#include "DictionaryDefinitionActivity.h"

#include <DictHtmlRenderer.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <SdDebugLog.h>
#include <Utf8.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <numeric>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/DictionaryRegistry.h"
#include "util/FlashcardDeck.h"
#include "util/IpaUtils.h"
#include "util/LookupHistory.h"
#include "util/PageTokenScan.h"
#include "util/TextPool.h"

static constexpr char kBullet[] = "- ";

namespace {

// Scan-pass accumulator for prewarmDefinitionFont(): the deduped set of codepoints the
// definition uses, as UTF-8, plus the styles it uses them in. Heap-allocated by the caller
// — the tables exceed the 256-byte stack budget, same reason SdCardFont::prewarm()
// heap-allocates its own codepoint buffer (SdCardFont.cpp:757).
//
// Body and IPA codepoints are collected separately because they are drawn with different
// fonts (see the isIpa branches in render/wrap): feeding IPA codepoints to the body font's
// prewarm only produces "glyph not found" misses, and the IPA font needs a prewarm of its
// own — see prewarmDefinitionFont().
//
// Dedup is split ASCII / non-ASCII deliberately: a definition is mostly ASCII, and the
// direct-indexed table keeps that path O(1) instead of the O(n^2) linear scan a single
// flat array would cost over several KB of text.
// Builds "<folderPath>.dict" into a caller-supplied buffer. The three call sites used to
// write `foundLocation.folderPath + ".dict"`, which heap-allocates (paths run ~60 chars,
// far past the SSO threshold) and, with -fno-exceptions, abort()s rather than returning
// null. Every one of them is on the definition screen, which device logs show running
// under 8KB free. 128 bytes matches Dictionary's own path buffers (Dictionary.cpp:93).
void buildDictPath(char* buf, size_t bufSize, const std::string& folderPath) {
  snprintf(buf, bufSize, "%s.dict", folderPath.c_str());
}

struct PrewarmCollector {
  static constexpr uint16_t MAX_NON_ASCII = 192;
  static constexpr uint8_t MAX_IPA = 64;
  static constexpr int CHUNK_SIZE = 512;

  bool seenAscii[128] = {};
  uint32_t nonAscii[MAX_NON_ASCII] = {};
  uint16_t nonAsciiCount = 0;
  uint16_t uniqueCount = 0;
  uint32_t ipaSeen[MAX_IPA] = {};
  uint8_t ipaCount = 0;
  uint8_t styleMask = 0;
  // Body bytes seen per style (index = SdCardFont style index). Only used to order the
  // budgeted prewarm below, so that if the heap runs out it is the least-used style that
  // goes without — not whichever happened to be last in the bitmask.
  uint32_t styleBytes[4] = {};

  // Fixed buffers, not std::string. Both are bounded by the dedup tables above — a
  // codepoint only ever reaches them once, and only if it fit in seenAscii/nonAscii/ipaSeen
  // — so a growing container was never needed to hold them. It was actively harmful: with
  // -fno-exceptions both reserve() and append()-past-capacity abort() instead of failing,
  // which is how a 513-byte std::string::reserve rebooted the device three times in one
  // session while the heap gates further down were correctly refusing to warm anything.
  // Sizing them from the caps also drops two heap allocations from a path that runs when
  // the heap is at its worst.
  // Derived from the tables rather than chosen, so raising a cap resizes the buffer with it
  // and the two cannot drift apart: at most 128 distinct ASCII at 1 byte each, plus
  // MAX_NON_ASCII distinct non-ASCII at the 4-byte UTF-8 maximum.
  static constexpr uint16_t UTF8_CAP = 128 * 1 + MAX_NON_ASCII * 4;
  static constexpr uint16_t IPA_CAP = MAX_IPA * 4;

  char utf8[UTF8_CAP + 1] = {};
  char ipaUtf8[IPA_CAP + 1] = {};
  uint16_t utf8Len = 0;
  uint16_t ipaLen = 0;
  // Read buffer for the plain-text scan; lives here rather than on the stack.
  // +4 for the incomplete UTF-8 sequence carried over from the previous chunk.
  char chunk[CHUNK_SIZE + 4] = {};

  // Bounds-checked appends. The caps are derived from the dedup tables, so overflow is
  // unreachable; the guards are here so that if a table cap is ever raised without raising
  // these, the effect is a dropped glyph that loads on demand rather than a buffer overrun.
  void appendBody(const char* bytes, size_t len) {
    if (utf8Len + len > UTF8_CAP) return;
    memcpy(utf8 + utf8Len, bytes, len);
    utf8Len = static_cast<uint16_t>(utf8Len + len);
    utf8[utf8Len] = '\0';
  }
  void appendIpa(const char* bytes, size_t len) {
    if (ipaLen + len > IPA_CAP) return;
    memcpy(ipaUtf8 + ipaLen, bytes, len);
    ipaLen = static_cast<uint16_t>(ipaLen + len);
    ipaUtf8[ipaLen] = '\0';
  }

  // Append every not-yet-seen codepoint in `text` (null-terminated) to utf8 / ipaUtf8.
  // Returns true if the text contains any body (non-IPA) codepoint, i.e. whether the
  // caller's style will actually be drawn in the body font.
  bool addText(const char* text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    bool sawBody = false;
    bool prevIsIpa = false;
    while (*p) {
      const unsigned char* seqStart = p;
      const uint32_t cp = utf8NextCodepoint(&p);
      if (cp == 0) break;
      const size_t seqLen = static_cast<size_t>(p - seqStart);
      const auto* seqBytes = reinterpret_cast<const char*>(seqStart);

      // Same run classification splitIpaRuns() applies at layout time (combining marks
      // inherit the current run), so both agree on which font draws each codepoint.
      const bool combining = utf8IsCombiningMark(cp);
      const bool isIpa = combining ? prevIsIpa : isIpaCodepoint(cp);
      prevIsIpa = isIpa;

      if (isIpa) {
        bool seen = false;
        for (uint8_t i = 0; i < ipaCount; i++) {
          if (ipaSeen[i] == cp) {
            seen = true;
            break;
          }
        }
        if (seen) continue;
        if (ipaCount >= MAX_IPA) continue;  // table full: let it fall back to the hot group
        ipaSeen[ipaCount++] = cp;
        appendIpa(seqBytes, seqLen);
        continue;
      }

      sawBody = true;
      // Cap matches SdCardFont's own per-prewarm limit: anything past it would be
      // dropped there anyway and is left to load on demand.
      if (uniqueCount >= SdCardFont::MAX_PAGE_GLYPHS) continue;
      if (cp < 128) {
        if (seenAscii[cp]) continue;
        seenAscii[cp] = true;
      } else {
        bool seen = false;
        for (uint16_t i = 0; i < nonAsciiCount; i++) {
          if (nonAscii[i] == cp) {
            seen = true;
            break;
          }
        }
        if (seen) continue;
        if (nonAsciiCount >= MAX_NON_ASCII) continue;  // table full: let it miss on demand
        nonAscii[nonAsciiCount++] = cp;
      }
      uniqueCount++;
      appendBody(seqBytes, seqLen);
    }
    return sawBody;
  }
};

// SdCardFont style index: 0 = REGULAR, 1 = BOLD, 2 = ITALIC, 3 = BOLD_ITALIC.
constexpr uint8_t spanStyleIndex(const bool bold, const bool italic) {
  return static_cast<uint8_t>((bold ? 1u : 0u) | (italic ? 2u : 0u));
}
constexpr uint8_t spanStyleBit(const bool bold, const bool italic) {
  return static_cast<uint8_t>(1u << spanStyleIndex(bold, italic));
}

// Mirror the dictionary's font-path timings into the SD debug log. LOG_DBG only reaches a
// serial monitor, but the reports that matter come from the device untethered, where
// opds_debug.txt is all there is — a "definition takes a minute to open" trace previously
// carried no timing data at all, only the Activity enter/exit markers.
//
// sd=0/1 is the key discriminator: the SD-font and built-in-font paths degrade through
// entirely different mechanisms. sd=1 with a high miss/missMs means the advance table or
// mini-data prewarm did not happen (overflow-ring thrash, one file open per glyph). sd=0
// with a high fdcOom/fdcMs means built-in group decompression is failing for contiguous
// heap instead. Stats are reset once per loadPage(), so wrap= is that phase alone and
// render= is cumulative over wrap+render — the same convention the LOG_DBG logStats lines
// already use.
// oom= counts glyph bitmaps that failed to allocate on the on-demand path; each one is a
// character drawText skipped, so a non-zero value here IS the "some fonts not rendering"
// report, rather than something to infer from a low largest= on the same line.
// displayMs/aaMs split the total for the "render" phase only; "wrap" leaves them 0. They exist
// because the X3 is USB-locked and the SD log is its only channel: the same numbers were already
// measured in render() but went to a serial-only LOG_DBG, so a 3461ms render could not be
// attributed. panel-bound (displayMs dominates) and compute-bound (neither does) call for
// completely different fixes, and the totals alone cannot tell them apart.
void logDictPhase(GfxRenderer& renderer, const int fontId, const char* phase, const unsigned long ms,
                  const unsigned long displayMs = 0, const unsigned long aaMs = 0) {
  uint32_t misses = 0, missMs = 0, bmpOom = 0, fdcSkips = 0, fdcMs = 0;
  const bool isSd = renderer.isSdCardFont(fontId);
  if (isSd) {
    const auto& fonts = renderer.getSdCardFonts();
    const auto it = fonts.find(fontId);
    if (it != fonts.end() && it->second) {
      misses = it->second->getStats().overflowMisses;
      missMs = it->second->getStats().overflowMissMs;
      bmpOom = it->second->getStats().bitmapOom;
    }
  }
  if (auto* fcm = renderer.getFontCacheManager()) {
    if (auto* fd = fcm->getDecompressor()) {
      fdcSkips = fd->getStats().hotGroupOomSkips;
      fdcMs = fd->getStats().decompressTimeMs;
    }
  }
  SdDebugLog::log(
      "DDA", "%s=%lums display=%lums aa=%lums sd=%d miss=%u missMs=%u oom=%u fdcOom=%u fdcMs=%u free=%u largest=%u",
      phase, ms, displayMs, aaMs, isSd ? 1 : 0, misses, missMs, bmpOom, fdcSkips, fdcMs,
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  // Start of an open. Stamped before the font work below, which is part of what the user waits
  // through. See openStartMs_ / openMs_.
  openStartMs_ = millis();
  openMs_ = 0;
  // Remember the dictionary in force on the way in so onExit() can put it back — a dictionary
  // picked here belongs to the word on screen, not to the reading session.
  // The RAW override, not activeDictPath(): an empty capture must restore to "no override"
  // rather than pinning the configured dictionary in as an explicit one.
  enterSessionDict_ = Dictionary::sessionDictPath();
  enterSessionDictWasPromotion_ = Dictionary::sessionPathIsFallbackPromotion();
  // The card's recorded dictionary, read once here rather than per render — cardDict() streams
  // the deck off SD. Keyed on historyWord for the same reason the Set action is (see
  // setCardDictToActive): it is the word the reader enrolled, which a switch does not change.
  cardDictExists_ = false;
  cardDictHash_ = 0;
  if (!cachePath.empty() && !historyWord.empty()) {
    cardDictExists_ = FlashcardDeck::cardDict(cachePath, historyWord, cardDictHash_);
  }
  // Heap reclaim: this activity is PUSHED on top of a still-resident reader
  // (ActivityManager keeps the backgrounded activity alive — no onExit). On the
  // tight X3 heap that leaves little headroom for the dictionary's own layout +
  // glyph decompress. Drop the reader's prewarmed font-cache page slots now; we
  // re-prewarm our own glyphs in wrapText() below, and the reader auto-re-prewarms
  // on its next render after this activity is popped. Self-healing, ~tens of KB.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  // "Same as book" follows the reader's FAMILY but keeps the dictionary's own size, so
  // that family has to be resident at the dictionary's point size before getDefinitionFontId()
  // can resolve to it. Must run before wrapText(), which caches the resolved id. Declines
  // itself when the size is already resident, the family ships no such file, or the heap
  // is too tight — the definition then renders at the book's size, as before.
  if (SETTINGS.dictionaryFontFamily == CrossPointSettings::DICT_FONT_MATCH_READER) {
    sdFontSystem.ensureFontSize(SETTINGS.getReaderSdFontFamilyName(), SETTINGS.getDefinitionPointSize(), renderer);
  }
  wrapText();
  // No forceCleanRefreshNextPaint() here any more. The screen this replaces may be carrying the
  // "Looking up" toast, whose box would otherwise sit under the incoming definition text — but
  // that is the TOAST's cost, so DictionaryLookupController::startLookup now sets the flag when
  // (and only when) it draws one. Doing it unconditionally here charged every definition for a
  // box that was not always on the glass: a scrub is ~730ms against ~437ms for a plain
  // differential, and the flag is one-shot, so an open with no toast now pays the difference back.
  //
  // What this does NOT cover, deliberately: the word-select highlight and a previous definition
  // being replaced. Both are ordinary full-content changes, and the dictionary-switch path
  // already repaints over exactly that with a plain FAST refresh (device logs: display=437ms, no
  // ghosting reported). Same hazard EpubReaderActivity::drawIndexingPopup() works around
  // (:490-493) — and it too sets the flag at the popup, not at the screen that follows.
  // immediate=true, and it matters. The default requestUpdate() only sets an atomic flag
  // (ActivityManager.cpp:330-334); the render task is not notified until ActivityManager::loop()
  // reaches :182-188, which happens after onEnter() RETURNS. So the history write below was not
  // overlapping the e-ink refresh as intended — it was running entirely in front of it. On the
  // slow LookupHistory path (a re-looked-up word, or history at cap: a full temp-file rewrite
  // with byte-at-a-time reads, LookupHistory.cpp:273-330) device logs measured ~2250ms of dead
  // time between the wrap finishing and render() starting, against ~255ms when the append fast
  // path was taken. That gap was the largest single component of a definition open.
  //
  // Safe to hand the render task the activity here: everything render() reads — layoutLines,
  // pagePool_, currentPage, totalPages, navigator — is settled by wrapText() above, and the only
  // state mutated after this point is chain_, which render() never touches. The history write and
  // any glyph-miss I/O the render issues both serialise on HalStorage's mutex by construction.
  requestUpdate(true);
  // Now genuinely concurrent with the refresh on the render task.
  const LookupHistory::WriteResult initialWrite =
      LookupHistory::addWordIf(cachePath, historyWord, historyStatus, recordHistory);

  // Seed the back-nav chain. The initial word is the newest history entry iff that write
  // actually landed — ask the write rather than re-deriving the conditions it applies
  // (history disabled, empty word, the stopword filter, an SD failure). A word with no
  // history slot cannot be referenced by an index, hence -1.
  chain_.reset(SETTINGS.getLookupHistoryCapValue());
  chain_.setCurrentHistIndex(initialWrite.wrote ? 0 : -1);
}

void DictionaryDefinitionActivity::onExit() {
  controller.onExit();  // stops+joins the lookup task first: nothing may free fonts under it
  // Undo onEnter()'s font residency. Symmetry here is the whole point: releaseExtraSizes()
  // used to run only from EpubReaderActivity::onExit(), so within one reading session the
  // dictionary's own .cpfont (interval + glyph-metadata tables), its four persistent advance
  // tables and its prewarmed mini bitmap arenas all stayed resident after the definition
  // closed — ~12KB sitting mid-heap, taken while the reader was still allocated.
  //
  // Device evidence (X3 opds_debug.txt, two lookups in one session): the reader's steady
  // state fell from `epub-page free=52112` to `free=40180`, and the second lookup entered at
  // free=16240/largest=11252 instead of 30768/24564. That was enough to make the body prewarm
  // decline every style (warmed=0x00), and since GfxRenderer::getTextWidth has no SD advance-
  // table fast path (GfxRenderer.cpp:538) the wrap then fetched every glyph individually
  // through onGlyphMiss — one file open + 2 seeks + 2 reads each, ~26ms apiece, 908 of them:
  // 25.3s to wrap what took 716ms the first time, with glyph bitmaps starting to fail to
  // allocate at largest=2036 (that is the "some fonts not rendering" report).
  //
  // Cost of releasing: the next lookup re-pays loadFile + the advance-table build, ~0.3s
  // (adv=137-162ms measured, plus one header/TOC read). Against 25s that is not a trade.
  // It also restores ensureFontSize()'s 28KB gate as a real per-lookup decision — once the
  // size is resident, getFontIdAtSize() returns early and the gate never runs again.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  // The size registration itself is NOT released here any more — the HOST activity that outlives
  // this one releases it on its own exit (DictionaryWordSelectActivity, LookedUpWordsActivity,
  // and the two flashcard screens, which already did). Two reasons the trailing comment above is
  // no longer sufficient:
  //
  //   - "word-select measures and renders through getReaderFontId() exclusively" stopped being
  //     true when the inline gloss box moved to the definition font (resolveGlossFont(), and its
  //     note at DictionaryWordSelectActivity.cpp:803-807). Releasing here forced word-select to
  //     re-run ensureFontSize on EVERY re-entry: gloss=620ms steady state, 904ms cold, i.e. half
  //     of the whole press-to-highlight wait.
  //   - The ~12KB regression this release was added to stop was dominated by the PREWARM
  //     products — four advance tables plus the mini bitmap arenas — and clearCache() above
  //     still frees those on every exit. What persists now is only the .cpfont size
  //     registration, and only for as long as a screen that needs it is on top.
  //
  // The invariant that actually matters is unchanged: the READER must never carry the extra size
  // (that is the free=52112 -> 40180 / warmed=0x00 / 25.3s wrap regression documented above), and
  // it still cannot, because every host releases before returning to it.
  //
  // Put back the dictionary that was in force on entry. Restoring the captured value rather
  // than clearing the override is what keeps a parent screen's per-card dictionary intact —
  // see enterSessionDict_. Safe here: the controller was stopped and joined at the top of this
  // function, so no lookup is in flight (Dictionary.h threading note).
  if (enterSessionDictWasPromotion_ && !enterSessionDict_.empty()) {
    Dictionary::promoteFallbackDictPath(enterSessionDict_.c_str());
  } else {
    Dictionary::setSessionDictPath(enterSessionDict_.c_str());
  }
  Activity::onExit();
}

int DictionaryDefinitionActivity::getLineHeight() const {
  return static_cast<int>(renderer.getLineHeight(defFontId_) * SETTINGS.getDefinitionLineCompression());
}

// ---------------------------------------------------------------------------
// Layout helpers — shared setup
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapText() {
  isWordSelectMode = false;
  navigator.reset();
  currentPage = 0;  // new definition always starts at page 0

  // Resolve the per-definition invariants once (see defFontId_ / defIsMarkup_).
  defFontId_ = SETTINGS.getDefinitionFontId();
  const DictInfo info = Dictionary::readInfo(foundLocation.folderPath.c_str());
  // 'h' is HTML, 'x' is XDXF. Both go to DictHtmlRenderer, which registers the two tag
  // vocabularies in one table (DictHtmlRenderer::classify). XDXF used to miss this test and
  // fall to wrapPlain(), which drew its markup on screen verbatim — tags, attributes and
  // undecoded &lt;/&gt; entities included.
  defIsMarkup_ = info.valid && (info.sametypesequence[0] == 'h' || info.sametypesequence[0] == 'x');

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  // Screen margin, composed the way the reader composes it (EpubReaderActivity.cpp:1896-1901):
  // the panel's physical viewable area first, then a constant inset on top. Before this the
  // definition viewer applied neither — a flat 5px on the sides only — which left ~2px actually
  // visible past the 3px side bezel and put the header band's top edge under the 9px top bezel,
  // while the screen it is opened from (DictionaryWordSelectActivity.cpp:521) already honoured
  // the bezel. The two disagreed.
  //
  // X3 (792x528) and X4 (800x480) share one binary and pick the panel at runtime
  // (FreeInkDisplay.h:9-10), so nothing here may assume a screen size: getOrientedViewableTRBL()
  // rotates the bezel per orientation, every value below is an inset measured from an edge, and
  // the two absolute dimensions come from getScreenWidth()/getScreenHeight(), which read the
  // runtime panel. Identical code path on both devices.
  //
  // The book's own margin, so a definition opened over a page keeps that page's gutters
  // instead of running out to the bezel beside it. getReaderScreenMargin() resolves the
  // per-book override before the global setting, which is what makes it the margin of the
  // book actually being read. Its minimum is 5, so a default configuration lays out exactly
  // as the fixed inset this replaced did; only a reader with widened margins sees a change.
  int bezelTop, bezelRight, bezelBottom, bezelLeft;
  renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
  const int screenMargin = SETTINGS.getReaderScreenMargin();

  // The landscape hint gutters stay inside these, so the side button hints are never overrun.
  contentX = bezelLeft + screenMargin + (isLandscapeCw ? hintGutterWidth : 0);
  leftPadding = contentX;
  rightPadding = bezelRight + screenMargin + (isLandscapeCcw ? hintGutterWidth : 0);
  contentTop = hintGutterHeight + bezelTop + screenMargin + metrics.topPadding;
  bodyStartY = contentTop + metrics.headerHeight + metrics.verticalSpacing;

  // Button hints are theme-owned chrome drawn at the panel edge on every screen in the app, so
  // they are not inset here; the margin instead keeps the last body line off them and off the
  // bottom bezel.
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing + bezelBottom + screenMargin;

  linesPerPage = (renderer.getScreenHeight() - bodyStartY - bottomArea) / getLineHeight();
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("DDA", "wrapText: font=%d sd=%d dictFamily=%u markup=%d linesPerPage=%d", defFontId_,
          renderer.isSdCardFont(defFontId_) ? 1 : 0, SETTINGS.dictionaryFontFamily, defIsMarkup_ ? 1 : 0, linesPerPage);

  // Must precede every measuring pass: loadPage() below measures the whole definition,
  // and so does every subsequent page turn (they call loadPage directly, so the glyphs
  // this warms have to stay resident for the life of the definition).
  prewarmDefinitionFont();

  loadPage(currentPage);
}

// ---------------------------------------------------------------------------
// Glyph prewarm (scan pass -> unique codepoints -> one batched load per font)
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::collectSpanForPrewarm(void* ctx, const StyledSpan& span) {
  auto* collector = static_cast<PrewarmCollector*>(ctx);
  if (!span.text) return;
  // Only claim the style if the span contributes body text — a purely-IPA span is drawn
  // in the IPA font, and claiming its style here would prewarm a whole extra style's
  // bitmaps in the body font for nothing.
  if (!collector->addText(span.text)) return;
  collector->styleMask |= spanStyleBit(span.bold, span.italic);
  collector->styleBytes[spanStyleIndex(span.bold, span.italic)] += strlen(span.text);
}

void DictionaryDefinitionActivity::prewarmDefinitionFont() {
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;

  // Built-in body fonts decompress into a RAM cache on first use, so they never pay per-glyph
  // SD I/O and the BODY half of this function is pure overhead for them.
  //
  // The IPA half is not. It used to sit behind an early return here, which meant a built-in
  // body font skipped the IPA prewarm as well — and since ipaWarm_ defaults to true, the IPA
  // font was still selected for drawing and every phonetic glyph came off the per-glyph
  // hot-group path. That path inflates a whole ~11KB group per group touched, GROWING its
  // buffer while still holding the previous one, and silently returns nullptr when the
  // contiguous block is not there ("OOM hot group ... glyph skipped", FontDecompressor.cpp:195).
  // The result is a phonetic transcription that renders correctly one time and drops a glyph
  // the next, with nothing on screen or in the log to say why: [reɪθ] became [reɪ ].
  const bool bodyIsSd = renderer.isSdCardFont(defFontId_);

  // Pessimistic from here: ipaWarm_ has to be earned at the gate below. Every early return
  // past this point (collector OOM, dictionary file unreadable) is itself evidence of a
  // starved heap, which is exactly when the per-glyph group inflation drops glyphs — so those
  // paths should land on the body-font fallback too.
  ipaWarm_ = false;

  const unsigned long t0 = millis();
  auto collector = makeUniqueNoThrow<PrewarmCollector>();
  if (!collector) {
    LOG_ERR("DDA", "OOM: prewarm collector (%u bytes)", static_cast<unsigned>(sizeof(PrewarmCollector)));
    return;  // not fatal: glyphs still load on demand, just slowly
  }
  // No reserve here: collector->utf8 is a fixed buffer sized from the dedup caps, so there
  // is nothing to grow and nothing that can abort. See PrewarmCollector's UTF8_CAP.
  char dictPath[128];
  buildDictPath(dictPath, sizeof(dictPath), foundLocation.folderPath);
  if (defIsMarkup_) {
    // Same streaming producer the wrap uses, with a collecting sink instead of the
    // measuring Wrapper — so the styles seen here are exactly the styles drawn later.
    const DictHtmlRenderer::SpanSink sink{collector.get(), &DictionaryDefinitionActivity::collectSpanForPrewarm};
    htmlRenderer_.renderFromFileStreaming(dictPath, foundLocation.offset, foundLocation.size, sink);
  } else {
    HalFile dictFile;
    if (!Storage.openFileForRead("DICT", dictPath, dictFile)) return;
    dictFile.seekSet(foundLocation.offset);

    uint32_t remaining = foundLocation.size;
    int carry = 0;  // bytes of an incomplete UTF-8 sequence held back from the last chunk
    while (remaining > 0) {
      const uint32_t space = static_cast<uint32_t>(PrewarmCollector::CHUNK_SIZE - carry);
      const uint32_t toRead = remaining < space ? remaining : space;
      const int n = dictFile.read(reinterpret_cast<uint8_t*>(collector->chunk) + carry, static_cast<int>(toRead));
      if (n <= 0) break;
      remaining -= static_cast<uint32_t>(n);

      const int total = carry + n;
      const int safe = utf8SafeTruncateBuffer(collector->chunk, total);
      // Save the straddling tail before the null terminator overwrites it, then move it
      // to the front for the next chunk.
      char tail[4] = {};
      const int tailLen = total - safe;
      memcpy(tail, collector->chunk + safe, static_cast<size_t>(tailLen));
      collector->chunk[safe] = '\0';
      // Plain text is all REGULAR; the IPA runs inside it still route to the IPA font.
      if (collector->addText(collector->chunk)) {
        collector->styleMask |= spanStyleBit(false, false);
        collector->styleBytes[spanStyleIndex(false, false)] += static_cast<uint32_t>(safe);
      }
      memcpy(collector->chunk, tail, static_cast<size_t>(tailLen));
      carry = tailLen;
    }
  }

  const unsigned long tScan = millis();
  if (collector->styleMask == 0) collector->styleMask = spanStyleBit(false, false);

  // Release the previous definition's glyph cache before allocating this one's. Two
  // reasons: FontDecompressor::prewarmCache() consumes a fresh page slot per call and
  // there are only MAX_PAGE_SLOTS (4) of them, and freeing first gives the allocations
  // below the largest contiguous blocks on an already-fragmented heap.
  fcm->clearCache();

  // Heap floors for the body prewarm below, declared up here because the IPA gate that
  // follows has to reason about the same budget. Two tiers: a uniform 16KB floor was
  // all-or-nothing (at 13.3KB free the loop broke before warming ANYTHING and the render then
  // paid 552 glyph-bitmap misses / 11.4s), so the most-used style — which by the sort below
  // covers the most text — is worth taking at a lower floor. One style is ~5KB; see the
  // 4-style figure in the loop comment.
  //
  // TRIED AND REVERTED (13KB/7KB): once the mini-bitmap budget stopped under-granting, lowering
  // this floor to admit more styles did nothing — device capture still showed warmed=0x04/0x05
  // with miss=58/78, because the loop then stopped on kMinBlockForStyle instead (post-prewarm
  // largest=6132-6900). It only cost 2KB of heap floor: minEver fell 10204 -> 8208. Style count
  // does correlate with misses, but it is not reachable by relaxing this gate.
  constexpr size_t kMinFreeForFirstStyle = 10 * 1024;
  constexpr size_t kMinBlockForFirstStyle = 6 * 1024;
  constexpr size_t kMinFreeForStyle = 16 * 1024;
  constexpr size_t kMinBlockForStyle = 8 * 1024;

  // IPA before the body font, deliberately. IPA runs are drawn with a built-in font whose
  // non-prewarmed path decompresses a whole ~11KB group per glyph (FontDecompressor.cpp:182);
  // once the body prewarm below has taken its share, that contiguous block no longer exists
  // and every IPA glyph is silently skipped ("OOM hot group ... glyph skipped"). Prewarming
  // it first, while the heap is least fragmented, both fixes that and drops the per-glyph
  // decompress. The IPA family is single-style (main.cpp:113), so 0x01 covers every style
  // the segments are drawn in.
  //
  // But first claim is not unconditional claim. A definition carries a handful of IPA
  // codepoints against dozens of body ones, and prewarmCache inflates a whole group
  // regardless, so on a tight heap the IPA font can outbid the body font and leave it with
  // nothing: the X3 log shows a 7-codepoint IPA prewarm followed by warmed=0x00 for 68 body
  // codepoints, which then cost 25.3s of per-glyph SD I/O. Only take IPA first if doing so
  // still leaves the top body style affordable. Declined IPA falls back to per-glyph
  // decompress — exactly what already happens whenever this prewarm fails.
  //
  // That gate tests the largest free BLOCK as well as total free heap, because what it is
  // authorising is a single contiguous malloc of group.uncompressedSize (11131 bytes for the
  // IPA font, FontDecompressor.cpp:487). On this activity's heap the two numbers diverge
  // badly — device logs routinely show free=15492 largest=9716 and free=9964 largest=4084 —
  // so a free-total gate happily passes an allocation that cannot succeed, and the failure
  // then resurfaces at draw time as dropped glyphs. Same rule as the body-style gates below
  // and as FontCacheManager.cpp:93-98.
  constexpr size_t kIpaGroupReserve = 12 * 1024;  // one FontDecompressor group, with margin
  const char* ipaOutcome = "none";
  if (collector->ipaLen != 0) {
    // Gate only when there is a body prewarm to protect this from. With a BUILT-IN body font
    // the loop below never runs, so nothing competes for the block and there is no threshold
    // worth setting: any number above the font's real group size declines a prewarm that would
    // have succeeded, and declining is what drops the glyphs. Attempting costs one small malloc
    // at worst — prewarmCache fails per group and keeps the glyphs it did extract, resets a
    // page slot it could not fill before claiming it, and leaves the rest on the same per-glyph
    // hot-group path that declining would have.
    bool attempt = true;
    if (bodyIsSd) {
      const size_t freeHeap = ESP.getFreeHeap();
      const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      attempt = freeHeap >= kIpaGroupReserve + kMinFreeForFirstStyle && largestBlock >= kIpaGroupReserve;
      if (!attempt) {
        ipaOutcome = "skip";
        LOG_DBG("DDA", "prewarm: skipping IPA (%u cp), free %u largest %u vs %u needed", collector->ipaCount,
                static_cast<unsigned>(freeHeap), static_cast<unsigned>(largestBlock),
                static_cast<unsigned>(kIpaGroupReserve));
      }
    }
    if (attempt) {
      // A prewarm that reports missed groups leaves those glyphs on the same failing
      // hot-group path, so only a clean 0 counts as warm.
      const int missed = fcm->prewarmCache(IPA_FONT_ID, collector->ipaUtf8, 0x01);
      ipaWarm_ = (missed == 0);
      ipaOutcome = ipaWarm_ ? "ok" : "miss";
    }
    // A BUILT-IN body font cannot stand in for the IPA font: it is a subset face carrying no
    // phonetic block, so falling back to it replaces the transcription with U+FFFD marks. Draw
    // from the IPA font whether or not the prewarm landed. Unwarmed it goes back through the
    // hot group, which is what this build already did -- still strictly better than a row of
    // question marks. The prewarm's value here is not permission to use the font, it is
    // getting the IPA groups OUT of the single hot-group slot that body glyphs are competing
    // for; that thrash is what fdcOom counts.
    //
    // An SD body font is the opposite case -- a full face that may well carry IPA itself -- so
    // it keeps the pessimistic rule above.
    if (!bodyIsSd) ipaWarm_ = true;
  }
  const unsigned long tIpa = millis();

  // Advance table for the body font, ALWAYS — it is the cheap half of font preparation
  // (8 B/codepoint, batched: one file open per style with glyph-index-sorted reads, and the
  // codepoint buffer borrows the inflate scratch window, SdCardFont.cpp:1351). Deliberately
  // ungated: affordable at any heap this activity can reach. No-op for built-in body fonts.
  // Runs after the IPA prewarm so that still gets first claim on a contiguous block.
  //
  // Scope: it feeds BOTH measuring fast paths, which is why the wrap no longer tracks
  // warmedMask. getTextAdvanceX (GfxRenderer.cpp:2023) takes pen advances from it, and
  // getSdInkWidth — the getTextWidth fast path — takes the bearing and width that
  // AdvanceEntry now carries in its padding, so an ink extent no longer needs a loaded
  // glyph either. Before that existed, every codepoint in a cold style fell into
  // SdCardFont::onGlyphMiss at ~26ms each and a 43-codepoint definition spent 2179ms of a
  // 2229ms wrap on SD I/O. Both fast paths skip kerning and ligatures; the styles the loop
  // below leaves cold now cost render time only, not measure time.
  if (bodyIsSd && collector->utf8Len != 0) {
    renderer.ensureSdCardFontReady(defFontId_, collector->utf8, collector->styleMask);
  }
  const unsigned long tAdv = millis();

  // Body font, one style at a time, most-used style first, stopping when the heap can no
  // longer afford the next one. Each style costs its own intervals, glyph array, bitmap
  // arena and mini kern matrix; prewarming all four of a 60-glyph definition took the X4
  // down to 5.9KB free / 2.1KB largest block, at which point the last style failed to
  // allocate anyway ("Failed to allocate mini bitmap") and everything downstream — the
  // wrap, pagePool_, the anti-aliasing pass — was running on fumes. A style left out here
  // still renders correctly; its glyphs just load on demand through the overflow ring.
  //
  // The two heap tiers are declared above the IPA gate, which budgets against them.
  // prewarmStyle fails gracefully at every allocation site (freeStyleMiniData + return, e.g.
  // SdCardFont.cpp:1015), so a floor set too low costs some wasted SD reads and lands back on
  // the on-demand path.
  uint8_t order[4] = {0, 1, 2, 3};
  for (uint8_t i = 1; i < 4; i++) {  // insertion sort by descending body bytes
    for (uint8_t j = i; j > 0 && collector->styleBytes[order[j - 1]] < collector->styleBytes[order[j]]; j--) {
      std::swap(order[j - 1], order[j]);
    }
  }

  uint8_t warmedMask = 0;
  // Built-in body fonts are deliberately left out: they cache on first use anyway, and each
  // prewarmCache call consumes one of the decompressor's few page slots — which the IPA
  // prewarm above needs more than they do.
  if (bodyIsSd && collector->utf8Len != 0) {
    for (const uint8_t styleIdx : order) {
      const uint8_t bit = static_cast<uint8_t>(1u << styleIdx);
      if (!(collector->styleMask & bit)) continue;
      const bool isFirst = warmedMask == 0;
      const size_t minFree = isFirst ? kMinFreeForFirstStyle : kMinFreeForStyle;
      const size_t minBlock = isFirst ? kMinBlockForFirstStyle : kMinBlockForStyle;
      if (ESP.getFreeHeap() < minFree || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < minBlock) {
        LOG_DBG("DDA", "prewarm: %s heap floor reached, styles 0x%02X left on demand", isFirst ? "first" : "next",
                static_cast<uint8_t>(collector->styleMask & ~warmedMask));
        break;
      }
      fcm->prewarmCache(defFontId_, collector->utf8, bit);
      warmedMask |= bit;
    }
  }

  LOG_DBG("DDA",
          "prewarm: body=%u ipa=%u mask=0x%02X warmed=0x%02X scan=%lums ipa=%lums adv=%lums body=%lums free=%u "
          "largest=%u",
          collector->uniqueCount, collector->ipaCount, collector->styleMask, warmedMask, tScan - t0, tIpa - tScan,
          tAdv - tIpa, millis() - tAdv, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  // warmed=0x00 here is the signature of a slow open: nothing prewarmed means every glyph
  // is fetched individually downstream. ipaWarm= is the phonetics equivalent. Read it with
  // sd=: at sd=1 anything but "ok" means the IPA segments fell back to the body font (see
  // ipaFontId()), while at sd=0 the IPA font is used regardless and a non-"ok" means those
  // glyphs are on the hot-group path -- watch fdcOom= on the render line for dropped ones.
  SdDebugLog::log("DDA",
                  "prewarm sd=%d body=%u ipa=%u ipaWarm=%s mask=0x%02X warmed=0x%02X adv=%lums body=%lums free=%u "
                  "largest=%u",
                  bodyIsSd ? 1 : 0, collector->uniqueCount, collector->ipaCount, ipaOutcome, collector->styleMask,
                  warmedMask, tAdv - tIpa, millis() - tAdv, static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

// Re-parse the definition and lay out ONLY `page` into layoutLines. The wrap
// produces every line, but collectLineSink keeps only this page's lines (the
// rest are produced then dropped, so peak RAM is one page, not the whole
// definition) and counts all lines to recompute totalPages. Called on entry and
// on every page turn (Stage 2a: re-parse every turn, both directions).
void DictionaryDefinitionActivity::loadPage(int page) {
  layoutLines.clear();
  pagePool_.clear();
  collectTargetPage_ = page;
  collectLineCount_ = 0;
  collectOom_ = false;

  // One reserve for the page's whole line budget, sized exactly. collectLineSink keeps only
  // indices in [start, start+linesPerPage), so with this capacity in hand its push_back can
  // never reallocate — which is what makes that push_back abort-free rather than merely
  // unlikely to abort. If even this fails, the wrap still runs (pagination needs the line
  // count) but pools nothing.
  if (!reserveNoThrow(layoutLines, static_cast<size_t>(linesPerPage) + 1)) {
    LOG_ERR("DDA", "OOM: page line budget (%d lines), page will render empty", linesPerPage);
    collectOom_ = true;
  }

  const unsigned long t0 = millis();
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->resetStats();  // attribute SD glyph I/O below to layout, not to the render

  // Choose rendering path based on dictionary content type (resolved in wrapText)
  if (defIsMarkup_) {
    wrapHtml();
  } else {
    wrapPlain();
  }

  totalPages = DictLayout::paginate(collectLineCount_, linesPerPage);

  LOG_DBG("DDA", "loadPage %d: wrap=%lums lines=%d pages=%d", page, millis() - t0, collectLineCount_, totalPages);
  if (collectOom_) {
    LOG_ERR("DDA", "page %d truncated: %d of %d line(s) pooled", page, static_cast<int>(layoutLines.size()),
            linesPerPage);
    SdDebugLog::log("DDA", "page %d TRUNCATED (OOM): pooled=%u/%d free=%u largest=%u", page,
                    static_cast<unsigned>(layoutLines.size()), linesPerPage, static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  }
  // The parser is live only for the wrap above — nothing between page turns touches it — so
  // holding it past this point is dead weight for as long as the definition is on screen.
  // ~7 KB on the markup path, which is what turned entering word-select into an abort():
  // extractWordsFromLayout()'s vector doubling asks for 4096 contiguous bytes and the largest
  // free block had fallen to 3444. Recreating on the next page turn costs ~1,952 B, noise
  // against the ~3.2 s that turn already spends in the panel refresh.
  htmlRenderer_.releaseParser();

  if (fcm) fcm->logStats("dict-wrap");
  logDictPhase(renderer, defFontId_, "wrap", millis() - t0);
}

void DictionaryDefinitionActivity::collectLineSink(void* ctx, DictLayout::LayoutLine&& line) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int idx = self->collectLineCount_++;
  const int start = self->collectTargetPage_ * self->linesPerPage;
  if (idx < start || idx >= start + self->linesPerPage) return;  // not on this page — discard

  // Every allocation below is on the definition-open path, which the device logs show
  // running at a few KB of free heap (largest block down to ~2KB). Under -fno-exceptions a
  // failed std::vector/std::string growth calls abort(), so the plain forms here would turn
  // a tight heap into a reboot mid-lookup. Each one now reports instead, and a line that
  // cannot be pooled in full is dropped whole — never pushed half-built, which would render
  // as a line with missing segments.
  //
  // collectLineCount_ was already incremented above, so pagination stays correct no matter
  // how many lines get dropped here; the page just renders short.
  if (self->collectOom_) return;  // page budget already blown — stop pooling, keep counting

  PooledLine pooled;
  pooled.indentLevel = line.indentLevel;
  pooled.isListItem = line.isListItem;
  if (!reserveNoThrow(pooled.segments, line.segments.size())) {
    self->collectOom_ = true;
    return;
  }
  for (const auto& seg : line.segments) {
    PooledSegment ps;
    if (!TextPool::appendNoThrow(self->pagePool_, seg.text.c_str(), seg.text.size(), ps.offset)) {
      self->collectOom_ = true;
      return;  // pagePool_ keeps whatever earlier segments wrote; this line is not recorded
    }
    ps.len = static_cast<uint16_t>(seg.text.size());
    ps.style = seg.style;
    ps.isIpa = seg.isIpa;
    pooled.segments.push_back(ps);  // exact-size reserve above: cannot reallocate
  }
  // Guard rather than trust: the range check above bounds this to linesPerPage entries and
  // loadPage() reserved that many, so capacity is there — but an explicit check is what makes
  // "no reallocation" a property of this code instead of an invariant two functions apart.
  if (self->layoutLines.size() >= self->layoutLines.capacity()) {
    self->collectOom_ = true;
    return;
  }
  self->layoutLines.push_back(std::move(pooled));
}

// ---------------------------------------------------------------------------
// Shared helper: measure text width accounting for mixed IPA/non-IPA runs
// ---------------------------------------------------------------------------

int DictionaryDefinitionActivity::getMixedWidth(std::vector<IpaTextSpan>& ipaRuns, const char* text,
                                                EpdFontFamily::Style style) {
  // Mirrors DictLayout::Wrapper::getMixedWidth: text without IPA is one non-IPA run, so
  // measure it directly rather than copying it onto a heap that has a few KB left.
  if (!text || !text[0]) return 0;
  if (!textHasIpa(text)) return renderer.getTextAdvanceX(defFontId_, text, style);
  ipaRuns.clear();
  if (!splitIpaRuns(text, ipaRuns)) {
    collectOom_ = true;
    return 0;
  }
  return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum + renderer.getTextAdvanceX(run.isIpa ? ipaFontId() : defFontId_, run.text.c_str(), style);
  });
}

// ---------------------------------------------------------------------------
// HTML path: run DictHtmlRenderer, lay out spans into LayoutLines
// ---------------------------------------------------------------------------

// Pen advance, NOT getTextWidth's ink extent -- renderBody() advances x by getTextAdvanceX, so
// measuring anything else lets the drawn line outrun the width it was wrapped to. Ink extent
// stops at the last inked pixel and drops the final glyph's right side bearing; the wrapper
// measures token by token, so a line accumulates one dropped bearing per token and its last
// word ends up hard against the right bezel. That also retires the single-space special case
// this used to carry: a space has no ink at all (getTextWidth returns 0 for " "), while its
// advance is exactly the gap the draw will step over.
int DictionaryDefinitionActivity::measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style,
                                                      bool isIpa) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int fontId = isIpa ? self->ipaFontId() : self->defFontId_;
  return self->renderer.getTextAdvanceX(fontId, text, style);
}

void DictionaryDefinitionActivity::wrapHtml() {
  const int maxWidth = renderer.getScreenWidth() - leftPadding - rightPadding;
  // Indent step: 3 spaces worth of pixels at regular weight. Advance, not ink extent, for the
  // reason measureWidthAdapter gives -- and spaces have no ink at all, so the ink form measured
  // two space advances instead of three.
  const int indentStep = renderer.getTextAdvanceX(defFontId_, "   ", EpdFontFamily::REGULAR);
  const int bulletWidth = renderer.getTextAdvanceX(defFontId_, kBullet, EpdFontFamily::REGULAR);

  // Fully streamed: the renderer delivers spans one at a time to the Wrapper, the
  // Wrapper emits completed lines to the page collector, and the collector keeps
  // only the current page. Neither the whole-definition span/textBuf (renderer)
  // nor all pages of lines (here) is ever materialized.
  DictLayout::Measurer measure{this, &DictionaryDefinitionActivity::measureWidthAdapter};
  DictLayout::LineSink lineSink{this, &DictionaryDefinitionActivity::collectLineSink};
  DictLayout::Wrapper wrapper(DictLayout::WrapMetrics{maxWidth, indentStep, bulletWidth}, measure, lineSink);

  // Renderer is a reused activity member (3.1-A): renderFromFileStreaming resets
  // it each call (XML_ParserReset, not free+create), so no per-turn object/parser
  // churn. Streaming means it never materializes the whole-definition buffers.
  char dictPath[128];
  buildDictPath(dictPath, sizeof(dictPath), foundLocation.folderPath);
  const DictHtmlRenderer::SpanSink spanSink{&wrapper, &DictionaryDefinitionActivity::feedSpanToWrapper};
  htmlRenderer_.renderFromFileStreaming(dictPath, foundLocation.offset, foundLocation.size, spanSink);
  wrapper.finish();
  // The wrapper's own transient strings are allocated on the same starved heap as everything
  // else here, and it stops rather than aborting when one fails. Folding its flag into
  // collectOom_ routes that into the truncation report loadPage() already emits — this is the
  // path that used to reboot the device (splitIpaRuns -> operator new(241) -> abort at 1220
  // bytes free).
  if (wrapper.oom()) collectOom_ = true;
  // Only the kept page's span text was ever copied into layoutLines.
}

void DictionaryDefinitionActivity::feedSpanToWrapper(void* ctx, const StyledSpan& span) {
  static_cast<DictLayout::Wrapper*>(ctx)->onSpan(span);
}

// ---------------------------------------------------------------------------
// Plain text path: word-wrap into single-segment REGULAR lines
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::wrapPlain() {
  std::vector<IpaTextSpan> ipaRuns;
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - leftPadding - rightPadding;
  const int spaceWidth = renderer.getSpaceWidth(defFontId_, EpdFontFamily::REGULAR);

  std::string currentWord;
  std::string currentLineText;
  int currentLineWidth = 0;

  DictLayout::LineSink sink{this, &DictionaryDefinitionActivity::collectLineSink};
  auto flushLine = [&]() {
    if (currentLineText.empty()) return;
    DictLayout::LayoutLine line;
    ipaRuns.clear();
    // A failed split leaves partial runs, which would render the line with text missing —
    // drop the segments and let the truncation report stand instead.
    if (!splitIpaRuns(currentLineText.c_str(), ipaRuns)) {
      collectOom_ = true;
      ipaRuns.clear();
    }
    // Sized exactly, so the push_back below cannot reallocate. Same reasoning as the sink:
    // an unguarded growth here aborts under -fno-exceptions, and this producer runs one
    // frame above collectLineSink — guarding only the sink would leave the reboot in place.
    if (!reserveNoThrow(line.segments, ipaRuns.size())) {
      collectOom_ = true;
    } else {
      // Move rather than copy: every consumer of ipaRuns clears it before use (here and in
      // getMixedWidth), so the runs are dead after this loop. Copying them allocated a second
      // std::string per segment — an unguarded growth, i.e. another abort site — for text that
      // was about to be discarded.
      for (auto& run : ipaRuns) {
        line.segments.push_back({std::move(run.text), EpdFontFamily::REGULAR, run.isIpa});
      }
    }
    // Emit even when empty: the sink increments collectLineCount_ before it inspects
    // anything, so pagination stays correct and the page renders a blank line rather than
    // silently renumbering itself.
    sink(std::move(line));
    currentLineText.clear();
    currentLineWidth = 0;
  };

  auto tryAppendWord = [&]() {
    if (currentWord.empty()) return;
    const int wordWidth = getMixedWidth(ipaRuns, currentWord.c_str(), EpdFontFamily::REGULAR);
    if (currentLineText.empty()) {
      currentLineText = currentWord;
      currentLineWidth = wordWidth;
    } else {
      const int testWidth = currentLineWidth + spaceWidth + wordWidth;
      if (testWidth <= maxWidth) {
        currentLineText += ' ';
        currentLineText += currentWord;
        currentLineWidth = testWidth;
      } else {
        flushLine();
        currentLineText = currentWord;
        currentLineWidth = wordWidth;
      }
    }
    currentWord.clear();
  };

  // Stream from .dict file — the full definition is never held in RAM.
  char dictPath[128];
  buildDictPath(dictPath, sizeof(dictPath), foundLocation.folderPath);
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath, dictFile)) return;
  dictFile.seekSet(foundLocation.offset);

  uint32_t remaining = foundLocation.size;
  char chunk[512];

  while (remaining > 0) {
    uint32_t toRead = remaining < sizeof(chunk) ? remaining : static_cast<uint32_t>(sizeof(chunk));
    int n = dictFile.read(reinterpret_cast<uint8_t*>(chunk), static_cast<int>(toRead));
    if (n <= 0) break;
    remaining -= static_cast<uint32_t>(n);

    for (int ci = 0; ci < n; ci++) {
      char c = chunk[ci];
      if (c == '\n') {
        tryAppendWord();
        flushLine();
      } else if (c == ' ') {
        tryAppendWord();
      } else {
        currentWord += c;
      }
    }
  }

  tryAppendWord();
  flushLine();
  dictFile.close();
}

// ---------------------------------------------------------------------------
// Word-select: extract words from the currently visible page
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::extractWordsFromLayout() {
  const unsigned long t0 = millis();

  // Pre-flight gate. Everything below allocates through std::vector / std::string, i.e. through
  // the THROWING operator new (HalSystem.cpp:100), which abort()s on failure under
  // -fno-exceptions — there is no nullptr to check and no way to unwind. A device crash landed
  // exactly here: the words vector doubled 64 -> 128 (32 B/entry = 4096 B) against
  // free=7012 largest=3444.
  //
  // A larger reserve is NOT the fix and would make it worse: a whole page is roughly
  // linesPerPage x ~14 words x 32 B, so reserving the worst case is a single ~8 KB block —
  // bigger than the allocation that already failed. Declining is the only safe answer.
  //
  // First estimates, same standing as kMinFreeForStyle above: sized to clear one 4096 B
  // doubling plus the text pool with margin. Tune from the logged values.
  constexpr size_t kMinFreeForWordSelect = 12 * 1024;
  constexpr size_t kMinBlockForWordSelect = 6 * 1024;
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < kMinFreeForWordSelect || largestBlock < kMinBlockForWordSelect) {
    LOG_ERR("DDA", "word-select declined: free=%u largest=%u (need %u/%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(largestBlock), static_cast<unsigned>(kMinFreeForWordSelect),
            static_cast<unsigned>(kMinBlockForWordSelect));
    SdDebugLog::log("DDA", "word-select declined: free=%u largest=%u", static_cast<unsigned>(freeHeap),
                    static_cast<unsigned>(largestBlock));
    // Tell the user, rather than making Confirm look dead. Same shape as the other toast sites
    // (EpubReaderActivity's mark-limit): drawPopup refreshes internally, hold it long enough to
    // read, then repaint the definition. The clean refresh stops the popup box ghosting under
    // the restored page. navigator stays empty, so loop() simply does not enter word-select.
    GUI.drawPopup(renderer, tr(STR_MEMORY_ERROR));
    delay(900);
    renderer.forceCleanRefreshNextPaint();
    requestUpdate();
    return;
  }

  const int indentStep = renderer.getTextAdvanceX(defFontId_, "   ", EpdFontFamily::REGULAR);

  std::vector<WordSelectNavigator::WordInfo> words;
  // Sized from the real page shape rather than a flat 64, because reaching 128 entries by
  // DOUBLING is the worst way to get there: it holds the old 2048 B buffer and the new 4096 B
  // one at the same time, needing 6 KB across two blocks. Reserving 128 up front is the same
  // end state through a single 4096 B allocation — and the gate above has already established
  // there is a 6 KB block to serve it. The cap keeps that first allocation at 4 KB; a page
  // needing more still grows, just from a base that covers the common case.
  words.reserve(std::min<size_t>(static_cast<size_t>(linesPerPage) * 12, 128));
  std::vector<WordSelectNavigator::Row> rows;
  rows.reserve(16);
  std::string textPool;
  textPool.reserve(512);

  const int lineHeight = getLineHeight();  // cached for loop
  for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
    const PooledLine& line = layoutLines[i];
    const int16_t lineY = static_cast<int16_t>(bodyStartY + i * lineHeight);
    int x = leftPadding + line.indentLevel * indentStep;

    if (line.isListItem) {
      x += renderer.getTextAdvanceX(defFontId_, kBullet, EpdFontFamily::REGULAR);
    }

    for (const auto& seg : line.segments) {
      const int segFontId = seg.isIpa ? ipaFontId() : defFontId_;
      const int spaceWidth = renderer.getSpaceWidth(segFontId, seg.style);
      const char* p = pagePool_.data() + seg.offset;
      while (*p) {
        while (*p == ' ') {
          x += spaceWidth;
          ++p;
        }
        if (!*p) break;

        const char* tokStart = p;
        while (*p && *p != ' ') ++p;
        const size_t tokLen = static_cast<size_t>(p - tokStart);
        std::string tok(tokStart, tokLen);

        const int tokAdvanceX = renderer.getTextAdvanceX(segFontId, tok.c_str(), seg.style);

        // One cursor stop per part, on the same dash rule the reading page uses, so
        // "word--word" is two selectable words here too. Unlike the reading page these
        // indices are not persisted anywhere, so the split is free of anchor concerns.
        PageTokens::Part parts[PageTokens::kMaxTokenParts];
        const size_t partCount = PageTokens::collectParts(tok.data(), tok.size(), parts, PageTokens::kMaxTokenParts);

        for (size_t pi = 0; pi < partCount; pi++) {
          const size_t partStart = parts[pi].start;
          const std::string part = tok.substr(partStart, parts[pi].end - partStart);
          std::string cleaned = Dictionary::cleanWord(part);
          if (cleaned.empty()) continue;
          // Only a split token needs its prefix measured to place the part; the common case
          // is one part at offset 0, where that measurement would be wasted work.
          const int partOffsetX =
              partStart == 0 ? 0 : renderer.getTextAdvanceX(segFontId, tok.substr(0, partStart).c_str(), seg.style);
          const int partWidth = renderer.getTextWidth(segFontId, part.c_str(), seg.style);
          uint16_t tokOff = WordSelectNavigator::poolAppend(textPool, part.c_str(), part.size());
          uint16_t cleanedOff = WordSelectNavigator::poolAppend(textPool, cleaned.c_str(), cleaned.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = tokOff;
          wi.textLen = static_cast<uint16_t>(part.size());
          wi.lookupOffset = cleanedOff;
          wi.lookupLen = static_cast<uint16_t>(cleaned.size());
          wi.screenX = static_cast<int16_t>(x + partOffsetX);
          wi.screenY = lineY;
          wi.width = static_cast<int16_t>(partWidth);
          wi.style = seg.style;
          wi.isIpa = seg.isIpa;
          words.push_back(wi);
        }
        x += tokAdvanceX;
      }
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
  LOG_DBG("DDA", "extractWords: %u words in %lums", static_cast<unsigned>(words.size()), millis() - t0);
  // WordInfo carries only the isIpa flag; the navigator resolves it against these two.
  navigator.setFonts(defFontId_, ipaFontId(), ipaBaselineOffset());
  navigator.load(std::move(words), std::move(rows), std::move(textPool));
}

// ---------------------------------------------------------------------------
// Input loop
// ---------------------------------------------------------------------------

bool DictionaryDefinitionActivity::handleLongPressExitAll(bool enabled) {
  if (enabled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    setResult(ActivityResult{});
    finish();
    return true;
  }
  return false;
}

void DictionaryDefinitionActivity::revertDictSwitchIfPending() {
  // Gated on the flag: a plain in-definition word lookup can also be cancelled or come
  // back not-found, and must not undo an override the user set earlier and is happy with.
  if (!dictSwitchInProgress_) return;
  if (prevSessionDictWasPromotion_) {
    Dictionary::promoteFallbackDictPath(prevSessionDict_.c_str());
  } else {
    Dictionary::setSessionDictPath(prevSessionDict_.c_str());
  }
  dictSwitchInProgress_ = false;
  prevSessionDictWasPromotion_ = false;
  prevSessionDict_.clear();
}

void DictionaryDefinitionActivity::restoreChainBackIfPending() {
  if (!chainBackNavInProgress) return;
  chainBackNavInProgress = false;
  // Puts the level back. The current history index needs no repair: pop() leaves it
  // alone, and the word on screen never changed.
  chain_.unpop(pendingBack_);
  pendingBack_ = {};
}

bool DictionaryDefinitionActivity::consumeDictSwitchRelease() {
  // Swallow the Confirm release left over from the press that fired the switch, so it
  // doesn't also fall through and open word-select -- or, once the select mode is open,
  // get read as the Confirm that accepts and closes it on the very same lift.
  //
  // Clearing is driven by the isPressed LEVEL, not the wasReleased EDGE. The edge is
  // unreliable here: startLookup() makes the controller active, so loop() returns at its
  // isActive() branch for the next several frames and never reaches this function — the
  // user's release lands inside that window and is gone by the time we look. Waiting for
  // an edge that already fired left the flag stuck true, and this function then returned
  // true forever, swallowing every input including Back. Same shape as
  // DictionaryWordSelectActivity's consumeInitialBackRelease_ (line 392).
  if (dictSwitchReleaseConsumed_) {
    const bool released = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      dictSwitchReleaseConsumed_ = false;
      // Only consume the frame if the release actually landed on it; otherwise fall
      // through so this frame's other input (Back, page turns) is still handled.
      return released;
    }
    return true;  // still physically held — keep swallowing
  }
  return false;
}

bool DictionaryDefinitionActivity::cycleDictionary() {
  // Nothing to cycle to with 0 or 1 dictionaries installed.
  if (dictionaryRegistry.count() < 2) return false;

  // Only mutate the session path while no lookup task is running — see the threading
  // note on Dictionary::setSessionDictPath.
  if (controller.isActive()) return false;

  const std::string current = Dictionary::activeDictPath(cachePath.empty() ? nullptr : cachePath.c_str());
  const int curIdx = dictionaryRegistry.indexOf(current);
  // Partitioned by folder name (DictionaryRegistry::nameIsStGroup): an "st-" dictionary
  // cycles only to another "st-", everything else only among themselves. Returns -1 when
  // this is the only dictionary in its group, and the guard below then leaves the screen
  // untouched.
  //
  // curIdx < 0 means the active dictionary is not in the registry (dictionary.bin empty, or
  // pointing at a folder discover() skipped as ambiguous). nextIndexInGroup has no group to
  // match then and falls back to index 0 — which is the ONE path that can cross the
  // partition. Decline instead: crossing it is exactly what this gesture prevents.
  const int nextIdx = curIdx < 0 ? -1 : dictionaryRegistry.nextEntryIndexInGroup(curIdx);
  if (nextIdx < 0) {
    SdDebugLog::log("DDA", "dict switch declined: cur=%d group=%s count=%d", curIdx,
                    curIdx < 0 ? "?" : (dictionaryRegistry.getEntries()[curIdx].nameIsSt ? "st" : "other"),
                    dictionaryRegistry.count());
    return false;
  }

  applyDictSwitch(curIdx, nextIdx, current);
  return true;
}

bool DictionaryDefinitionActivity::handleDictSwitch() {
  if (dictSwitchReleaseConsumed_ && consumeDictSwitchRelease()) return true;

  // Fire at the threshold rather than on release, so the gesture is distinguishable
  // from the short Confirm that opens word-select.
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS) {
    return false;
  }
  if (dictionaryRegistry.count() < 2 || controller.isActive()) return false;

  // Swallow the trailing release either way. Falling through on a DECLINED hop left
  // `wasReleased(Confirm)` to open word-select below — no heldTime check there — so a
  // declined long press acted like a short one instead of doing nothing, which is what
  // the gesture documents.
  dictSwitchReleaseConsumed_ = true;
  cycleDictionary();
  return true;
}

void DictionaryDefinitionActivity::applyDictSwitch(const int curIdx, const int nextIdx, const std::string& current) {
  prevSessionDict_ = current;
  // A fallback promotion is scoped to the entry on screen; restoring it as an explicit
  // path would make it outlive that entry, which is exactly what the scoping prevents.
  prevSessionDictWasPromotion_ = Dictionary::sessionPathIsFallbackPromotion();
  dictSwitchInProgress_ = true;
  Dictionary::setSessionDictPath(dictionaryRegistry.getEntries()[nextIdx].basePath.c_str());
  LOG_DBG("DDA", "dict switch -> %s", dictionaryRegistry.getEntries()[nextIdx].name.c_str());
  // To SD as well as serial: a wrong-group hop is only diagnosable from the pair of group
  // flags, and the SD log is what actually comes back from a device session.
  SdDebugLog::log("DDA", "dict switch: %s(%s) -> %s(%s)", dictionaryRegistry.getEntries()[curIdx].name.c_str(),
                  dictionaryRegistry.getEntries()[curIdx].nameIsSt ? "st" : "other",
                  dictionaryRegistry.getEntries()[nextIdx].name.c_str(),
                  dictionaryRegistry.getEntries()[nextIdx].nameIsSt ? "st" : "other");
  // recordHistory=false: the word is already in history from the original lookup.
  controller.startLookup(headword, false);
}

bool DictionaryDefinitionActivity::setOfferStands(const uint32_t activeHash) const {
  // No card to point anywhere, or no book to hold one.
  if (!cardDictExists_ || cachePath.empty() || historyWord.empty()) return false;
  // historyWord names the word the reader enrolled. It goes stale the moment the user chains
  // forward to another word from inside a definition, and chained words were never enrolled
  // (enrollment only happens at the reader's word-select gesture) — so offering here would
  // repoint the ORIGINAL card. pop()/unpop() bring depth back to 0, which re-arms the offer.
  if (chain_.depth() != 0) return false;
  // Nothing to commit when the card already names this dictionary. A card recording 0 (a
  // legacy card, or one enrolled before the association existed) DOES stand: that is the only
  // way those cards ever get stamped.
  return activeHash != 0 && activeHash != cardDictHash_;
}

bool DictionaryDefinitionActivity::setCardDictToActive() {
  const uint32_t activeHash = DictUtils::activeDictHash(cachePath.empty() ? nullptr : cachePath.c_str());
  if (!setOfferStands(activeHash)) return false;
  // Keyed on historyWord, NOT controller.getLookupWord(): the two are equal on the way in, but
  // a switch re-looks-up `headword`, so afterwards getLookupWord() holds the FOUND word. Look
  // "running" up, let a dictionary answer "run", and this would name "run" while the card is
  // filed under "running" — setCardDict would silently miss.
  if (!FlashcardDeck::setCardDict(cachePath, historyWord, activeHash)) {
    // Two different failures share that false: the card went away underneath us (deleted from
    // the list, or by a sync), or the deck rewrite itself failed. Ask which. Retiring the offer
    // on an I/O failure would look exactly like success -- the offer disappearing IS the
    // confirmation -- so leave it standing to be retried, and only drop it when the card is
    // genuinely gone.
    uint32_t stillRecorded = 0;
    cardDictExists_ = FlashcardDeck::cardDict(cachePath, historyWord, stillRecorded);
    LOG_ERR("DDA", "set card dict failed for '%s' (card %s)", historyWord.c_str(),
            cardDictExists_ ? "present" : "gone");
    SdDebugLog::log("DDA", "card dict set FAILED: %s (card %s)", historyWord.c_str(),
                    cardDictExists_ ? "present" : "gone");
    requestUpdate();
    return true;  // the offer stood when pressed, so the press was still ours
  }
  cardDictHash_ = activeHash;
  SdDebugLog::log("DDA", "card dict set: %s -> %lu", historyWord.c_str(), static_cast<unsigned long>(activeHash));
  // The offer disappearing IS the confirmation — it is drawn from the state this just changed.
  requestUpdate();
  return true;
}

void DictionaryDefinitionActivity::loop() {
  // --- Controller active (LookingUp / AltFormPrompt / NotFound) ---
  if (controller.isActive()) {
    const DictionaryLookupController::LookupEvent event = controller.handleInput();
    // A miss the controller recorded (DictionaryLookupController::handleLookupFailed) grew the
    // history log without any navigation, so every chain index moved. Re-index before
    // anything reads them, or the next Back resolves one slot too new.
    if (const LookupHistory::WriteResult missWrite = controller.takeHistoryWrite(); missWrite.wrote) {
      chain_.onHistoryWrite(missWrite.prevIndex);
    }
    switch (event) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        const bool wasBackNav = chainBackNavInProgress;
        // A dictionary switch re-resolves the SAME word, so it is not navigation:
        // no chain entry, no history write, no page restore. It only re-wraps.
        const bool wasDictSwitch = dictSwitchInProgress_;
        // The page the word being left is on, captured before wrapText() below resets it.
        // The chain entry for it is pushed after the history write, which is what decides
        // how the indices move.
        const uint16_t leftOnPage = static_cast<uint16_t>(currentPage);
        chainBackNavInProgress = false;
        dictSwitchInProgress_ = false;
        prevSessionDict_.clear();
        prevSessionDictWasPromotion_ = false;
        headword = controller.getFoundWord();
        foundLocation = controller.getFoundLocation();
        // A chained lookup is a new definition, so it re-measures. The other stamp site is
        // onEnter(); a page turn is deliberately neither. See openStartMs_ / openMs_.
        openStartMs_ = millis();
        openMs_ = 0;
        wrapText();  // resets currentPage to 0 and loads page 0
        if (wasBackNav) {
          // Re-derive the now-current word's history position and restore its page.
          chain_.setCurrentHistIndex(pendingBack_.histIndex);
          currentPage = (pendingBack_.page < totalPages) ? pendingBack_.page : (totalPages - 1);
          if (currentPage < 0) currentPage = 0;
          if (currentPage > 0) loadPage(currentPage);
        }
        isWordSelectMode = false;
        // immediate=true for the same reason as onEnter(): the history write below would
        // otherwise run in front of the render rather than beside it. Same safety argument —
        // every field render() reads is settled by wrapText()/loadPage() above, and the switch
        // returns straight after this case with nothing else touched.
        requestUpdate(true);
        // Chain-forward records; chain-back-nav and dictionary switches do not (both
        // re-resolve a word that is already in history).
        const LookupHistory::WriteResult write =
            LookupHistory::addWordIf(cachePath, controller.getLookupWord(),
                                     DictionaryLookupController::toHistStatus(controller.getFoundStatus()),
                                     !wasBackNav && !wasDictSwitch && controller.getRecordHistory());
        // Forward: push a back-entry for the word being left, referencing its history
        // position. Driven by what the write actually did rather than by a re-derived
        // "will this be logged?" — the log skips stopwords and MOVES a word it already
        // holds, and each of those shifts the indices differently.
        if (!wasBackNav && !wasDictSwitch) chain_.onForward(leftOnPage, write.wrote, write.prevIndex);
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        // The previous definition is still on screen. If a dictionary switch is what
        // failed, put the old dictionary back so the footer label and the body agree.
        revertDictSwitchIfPending();
        restoreChainBackIfPending();
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        // Done closes the screen, so there is no stale body to disagree with the override.
        // No need to revert either: onExit() restores the dictionary that was in force when
        // this screen opened, so the choice cannot outlive the word it was made for.
        dictSwitchInProgress_ = false;
        prevSessionDict_.clear();
        prevSessionDictWasPromotion_ = false;
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        revertDictSwitchIfPending();
        restoreChainBackIfPending();
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  // --- Word-select mode ---
  if (isWordSelectMode) {
    if (navigator.handleNavigation(mappedInput, renderer, SETTINGS.getReaderSwapWordSelectAxes())) {
      requestUpdate();
    }

    if (controller.handleMultiSelect(navigator)) return;

    if (!navigator.isMultiSelecting()) {
      if (controller.handleConfirmLookup(navigator)) return;

      if (handleLongPressExitAll(true)) return;

      // Short press Back: exit word-select mode.
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
          mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS) {
        isWordSelectMode = false;
        navigator.reset();
        requestUpdate();
      }
    }
    return;
  }

  // --- View mode ---
  // Right doubles as "set this dictionary on the card" while the offer stands, so it is
  // tested before the paging block below — otherwise the same release would also turn the
  // page. Only the Right ALIAS is taken: the PageForward side button and the right-two-thirds
  // tap still page, which is what keeps a multi-page definition navigable after a switch.
  //
  // Gated on the release FIRST: setCardDictToActive() resolves the active dictionary path,
  // which allocates, and this runs on every frame of the input loop. It returns false when the
  // offer does not stand — the common case, since a freshly looked-up word's card already names
  // the dictionary on screen — and the press then falls through to page forward as usual.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && cardDictExists_ && chain_.depth() == 0 &&
      setCardDictToActive()) {
    return;
  }

  const bool prevPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (prevPage && currentPage > 0) {
    currentPage--;
    loadPage(currentPage);
    requestUpdate();
  }

  if (nextPage && currentPage < totalPages - 1) {
    currentPage++;
    loadPage(currentPage);
    requestUpdate();
  }

  // Touch page-turn (view mode): tap left third = previous page, the rest = next.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    // Both footer controls are tested before the page-turn thirds below, which would
    // otherwise swallow them: they sit bottom-left, inside the "previous page" third. These
    // are the ONLY way either action is reachable on a board with no Confirm button to hold
    // and no Right button to press (BoardConfig.h, XTEINK_X4_PRO).
    //
    // The Set chip is tested first because it is drawn to the right of the name and the two
    // rectangles are padded generously enough to abut.
    const auto hit = [&](int x, int y, int w, int h) {
      return w > 0 && tx >= x && tx < x + w && ty >= y && ty < y + h;
    };
    if (!controller.isActive() && hit(setChipX_, setChipY_, setChipW_, setChipH_)) {
      setCardDictToActive();
      return;
    }
    if (!controller.isActive() && hit(dictLabelX_, dictLabelY_, dictLabelW_, dictLabelH_)) {
      if (!cycleDictionary()) return;  // sole member of its group: consume the tap, do nothing
      // Boards with touch.synthConfirm turn a tap into a Confirm as well. Swallow that one the
      // same way the long-press gesture does, so the tap that cycles cannot also fall through
      // and open word-select. Harmless where no synthetic Confirm follows: the latch clears on
      // the first frame with none pending.
      dictSwitchReleaseConsumed_ = true;
      return;
    }
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        currentPage--;
        loadPage(currentPage);
        requestUpdate();
      }
    } else if (currentPage < totalPages - 1) {
      currentPage++;
      loadPage(currentPage);
      requestUpdate();
    }
    return;
  }

  // Long-press Confirm: cycle the session dictionary. Checked before the release
  // handler below so the switch wins over word-select on a held Confirm.
  if (handleDictSwitch()) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (showLookupButton) {
      extractWordsFromLayout();
      if (!navigator.isEmpty()) {
        isWordSelectMode = true;
        requestUpdate();
      }
    } else {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  if (handleLongPressExitAll(showLookupButton)) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      (!showLookupButton || mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS)) {
    if (!cachePath.empty() && !chain_.empty()) {
      pendingBack_ = chain_.pop();
      // Resolve the prior headword from the persisted history by distance-from-newest.
      // Streaming single-word fetch: the reader is still resident underneath us and
      // the heap is at its most fragmented here -- materializing the whole history
      // (the old LookupHistory::load call) could OOM-abort.
      const std::string priorWord = LookupHistory::getWordNewestFirst(cachePath, pendingBack_.histIndex);
      if (!priorWord.empty()) {
        chainBackNavInProgress = true;
        controller.startLookup(priorWord, false);
        return;
      }
      // Unresolvable (should not happen under the depth cap) — fall through to exit.
    }
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DictionaryDefinitionActivity::render(RenderLock&&) {
  // Differential fast path: only when we're already in word-select mode AND
  // we set it up on the previous frame AND the controller has nothing pending.
  if (isWordSelectMode && nextRenderMode_ == RenderMode::Differential && !controller.isActive()) {
    const int currIdx = navigator.getCurrentFlatIndex();
    if (currIdx >= 0) {
      const int lineHeight = getLineHeight();
      auto dirty = navigator.renderHighlightDifferential(renderer, lineHeight, prevHighlightIdx_, currIdx);
      if (dirty.has_value()) {
        // Full panel push — matches DictionaryWordSelectActivity. Windowed refresh is not
        // wired up because the SDK's experimental path produces alternating black→white
        // failures on consecutive partial refreshes. Savings come from skipping page->render.
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        prevHighlightIdx_ = currIdx;
        return;
      }
      // fall through to full repaint path
    }
  }

  // Full repaint path.
  const unsigned long t0 = millis();
  renderer.clearScreen();
  if (controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }

  const auto metrics = UITheme::getInstance().getMetrics();
  const int indentStep = renderer.getTextAdvanceX(defFontId_, "   ", EpdFontFamily::REGULAR);

  // Header
  // Width spans between the two insets rather than the full screen, so the header band carries
  // the same margin as the body. In portrait both insets are equal, which keeps the band
  // symmetric about the screen-centred clock BaseTheme::drawTopBarClockDate draws into it.
  const char* headerText = headword.c_str();
#if LOG_LEVEL >= 2
  // Diagnostic: annotate the searched word with what opening this definition cost. Pinned to the
  // open (see openStartMs_), so paging up/down does not overwrite it with a page turn's ~0.5 s.
  //
  // The figure the user waits through cannot be known here: everything below this line — the body
  // draw, the panel refresh, the AA pass — is still ahead, and the panel refresh alone is the
  // dominant term (device: display=3196ms of a 3439ms total). Redrawing the header after the fact
  // would cost a second full-panel refresh. So the FIRST render of a definition shows an estimate,
  // marked `~`: elapsed-so-far plus what the same tail cost on the previous definition's first
  // frame. That tail is panel time, i.e. a property of the display rather than of the entry, so it
  // carries across definitions well; one static unsigned long in dev builds pays for it. From the
  // second render onwards the exact measured openMs_ replaces the estimate.
  //
  // Seconds to HUNDREDTHS, rounded — "4.06s". Tenths were tried and are useless here: the panel
  // refresh dominates and barely varies, so every open quantised to the same "4.0s". Two decimals
  // keep the ~250 ms that does vary between words visible while still reading as a duration.
  //
  // Stack buffer, not std::string: this is a render path. No tr() — a bare "(4.06s)" carries
  // no language. Dev builds only; release is LOG_LEVEL=1 and drops the whole thing.
  static unsigned long sPostHeaderMs = 0;
  const unsigned long tHeader = millis();
  char headerBuf[96];
  if (openMs_ > 0 || sPostHeaderMs > 0) {
    const bool measured = openMs_ > 0;
    const unsigned long ms = measured ? openMs_ : (tHeader - openStartMs_) + sPostHeaderMs;
    const unsigned long cs = (ms + 5) / 10;  // hundredths, rounded: 4056ms -> 406 -> "4.06"
    snprintf(headerBuf, sizeof(headerBuf), "%s (%s%lu.%02lus)", headword.c_str(), measured ? "" : "~", cs / 100,
             cs % 100);
    headerText = headerBuf;
  }
#endif
  GUI.drawHeader(renderer,
                 Rect{contentX, contentTop, renderer.getScreenWidth() - contentX - rightPadding, metrics.headerHeight},
                 headerText);

  // Body: draw layout lines for the current page (BW pass). layoutLines holds
  // only the current page (Stage 2a streaming), so it is indexed from 0.
  const int lineHeight = getLineHeight();  // cached for loop + renderHighlight
  const int ipaDy = ipaBaselineOffset();   // cached for loop; shares the body font's baseline
  auto renderBody = [&]() {
    for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
      const PooledLine& line = layoutLines[i];
      const int y = bodyStartY + i * lineHeight;
      int x = leftPadding + line.indentLevel * indentStep;

      if (line.isListItem) {
        renderer.drawText(defFontId_, x, y, kBullet);
        x += renderer.getTextAdvanceX(defFontId_, kBullet, EpdFontFamily::REGULAR);
      }

      for (const auto& seg : line.segments) {
        const int segFontId = seg.isIpa ? ipaFontId() : defFontId_;
        const int segY = seg.isIpa ? y + ipaDy : y;
        const char* segText = pagePool_.data() + seg.offset;
        renderer.drawText(segFontId, x, segY, segText, true, seg.style);
        if ((seg.style & EpdFontFamily::UNDERLINE) != 0) {
          const int segWidth = renderer.getTextWidth(segFontId, segText, seg.style);
          const int underlineY = segY + renderer.getFontAscenderSize(segFontId) + 2;
          renderer.drawLine(x, underlineY, x + segWidth, underlineY, true);
        }
        x += renderer.getTextAdvanceX(segFontId, segText, seg.style);
      }
    }

    // Truncation notice. Without it a definition that ran out of heap mid-layout is
    // indistinguishable from a genuinely short entry — the reader has no way to tell that text
    // is missing, or that backing out and reopening on a calmer heap would show more. Drawn
    // outside layoutLines, so it is not word-selectable and the highlight navigator's indices
    // are unaffected.
    //
    // REGULAR deliberately, not italic: on an SD-card font only the styles prewarmDefinitionFont
    // could afford are resident, and by the time this notice is drawn the heap is by definition
    // too small to load a cold style's glyphs on demand — an italic notice would render as a row
    // of missing-glyph marks, which is worse than no notice at all.
    if (collectOom_) {
      const int noticeRow = static_cast<int>(layoutLines.size());
      if (noticeRow < linesPerPage) {
        renderer.drawText(defFontId_, leftPadding, bodyStartY + noticeRow * lineHeight, tr(STR_ERROR_LOW_MEMORY), true,
                          EpdFontFamily::REGULAR);
      }
    }
  };
  renderBody();
  const unsigned long tBody = millis();

  // Word-select mode: overlay highlighted word(s) and prime snapshot for next frame.
  // The -1 prevWordIdx literal is load-bearing: renderHighlightDifferential uses
  // prevWordIdx < 0 as the signal "framebuffer was just redrawn from scratch,
  // discard any stale snapshot rather than restoring it on top of fresh pixels."
  // This is the only path that disturbs the framebuffer outside the differential
  // cycle, so it's also the only call site that must pass -1.
  if (isWordSelectMode) {
    const int currIdx = navigator.getCurrentFlatIndex();
    bool snapshotPrimed = false;
    if (currIdx >= 0) {
      auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
      snapshotPrimed = setup.has_value();
    }
    if (!snapshotPrimed) {
      navigator.renderHighlight(renderer, lineHeight);
    }

    // Empty button hints in word-select mode (same convention as EPUB word-select)
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    LOG_DBG("DDA", "render(select): body=%lums total=%lums", tBody - t0, millis() - t0);
    if (auto* fcm = renderer.getFontCacheManager()) fcm->logStats("dict-render");

    prevHighlightIdx_ = currIdx;
    nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
    return;
  }

  // View mode: differential state is irrelevant — reset so that the next entry
  // into word-select starts cleanly with a full repaint.
  nextRenderMode_ = RenderMode::FullPage;
  prevHighlightIdx_ = -1;

  // Pagination indicator and button hints
  const int footerY = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (totalPages > 1) {
    char pageInfo[16];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage + 1, totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth, footerY, pageInfo);
  }

  // Active dictionary, bottom-left, mirroring the pagination indicator opposite it. Shown
  // only when there is something to switch between: it is both the feedback for the
  // long-press-Confirm cycle and the touch control for it.
  const std::string activePath = Dictionary::activeDictPath(cachePath.empty() ? nullptr : cachePath.c_str());
  // Hash the path already resolved above rather than calling activeDictHash(), which would
  // resolve and allocate a second copy of it on every paint.
  const bool offerSet = setOfferStands(DictUtils::dictHashOfPath(activePath));
  constexpr int kTouchPad = 12;  // a small label; a finger is not
  const int labelH = renderer.getTextHeight(SMALL_FONT_ID);
  int cursorX = leftPadding;
  dictLabelW_ = 0;
  setChipW_ = 0;
  if (dictionaryRegistry.count() > 1) {
    const std::string dictName = DictUtils::dictDisplayName(activePath);
    renderer.drawText(SMALL_FONT_ID, cursorX, footerY, dictName.c_str());
    // Remember where it landed so a tap on it can cycle. Padded generously: nothing else is
    // drawn along that edge.
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, dictName.c_str());
    dictLabelX_ = cursorX - kTouchPad;
    dictLabelY_ = footerY - kTouchPad;
    dictLabelW_ = labelW + 2 * kTouchPad;
    dictLabelH_ = labelH + 2 * kTouchPad;
    // Underline it on touch boards so it reads as a control rather than a status line.
    if (mappedInput.hasTouch()) {
      renderer.drawLine(cursorX, footerY + labelH + 1, cursorX + labelW, footerY + labelH + 1, true);
    }
    cursorX += labelW + 2 * kTouchPad;
  }

  // The Set chip, drawn only on a board with no Right button to press: there it is the sole
  // way to reach the action. Where Right exists (X3, X4) the button-hint slot below already
  // names it, and a second on-screen label for the same action is just clutter.
  //
  // Outside the count() > 1 block deliberately: with a single dictionary installed there is
  // nothing to cycle to, but a legacy card recording no dictionary still needs a way to be
  // stamped with the one in use.
  if (offerSet && !mappedInput.isAvailable(MappedInputManager::Button::Right)) {
    const char* setLabel = tr(STR_SET_CARD_DICT);
    const int chipW = renderer.getTextWidth(SMALL_FONT_ID, setLabel);
    renderer.drawText(SMALL_FONT_ID, cursorX, footerY, setLabel);
    // Boxed rather than underlined: it has to read as a distinct control from the dictionary
    // name beside it, on non-touch boards too.
    renderer.drawRect(cursorX - 3, footerY - 2, chipW + 6, labelH + 4, true);
    setChipX_ = cursorX - kTouchPad;
    setChipY_ = footerY - kTouchPad;
    setChipW_ = chipW + 2 * kTouchPad;
    setChipH_ = labelH + 2 * kTouchPad;
  }

  // Confirm label only — Back and the Up/Down page labels are left empty so this
  // matches the word-select screen that precedes it, which draws hints the same way
  // (DictionaryWordSelectActivity.cpp:587). The buttons themselves are unaffected:
  // Back still exits/chains back and Up/Down still page. Paging stays discoverable
  // via the "n/m" indicator drawn opposite the dictionary name above.
  // While the Set offer stands, Right performs it instead of paging, so name it in the "next"
  // slot — that slot is free in view mode precisely because paging is left unlabelled.
  const char* btn2 = showLookupButton ? tr(STR_LOOKUP_SHORT) : "";
  const auto labels = mappedInput.mapLabels("", btn2, "", offerSet ? tr(STR_SET_CARD_DICT) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  const unsigned long tDisplay = millis();

  // Anti-aliasing pass: overlay grayscale body text on top of the BW display
  if (SETTINGS.textAntiAliasing == CrossPointSettings::TEXT_AA_ANTIALIASED) {
    ReaderUtils::renderAntiAliased(renderer, renderBody);
  }

  LOG_DBG("DDA", "render: body=%lums display=%lums aa=%lums total=%lums", tBody - t0, tDisplay - tBody,
          millis() - tDisplay, millis() - t0);
  if (auto* fcm = renderer.getFontCacheManager()) fcm->logStats("dict-render");
  // First completed render since the definition opened: everything the user waited through,
  // panel refresh and AA pass included. Page turns leave it alone — that is the point.
  if (openMs_ == 0) {
    openMs_ = millis() - openStartMs_;
#if LOG_LEVEL >= 2
    // Sample the post-header tail from a FIRST frame only, so the estimate drawn on the next
    // definition's first frame predicts like for like (a page turn's tail carries no glyph misses).
    sPostHeaderMs = millis() - tHeader;
#endif
  }
  logDictPhase(renderer, defFontId_, "render", millis() - t0, tDisplay - tBody, millis() - tDisplay);
}
