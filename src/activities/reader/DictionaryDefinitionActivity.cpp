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
#include "util/DictStopwords.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/IpaUtils.h"
#include "util/LookupHistory.h"
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
  std::string utf8;
  std::string ipaUtf8;
  // Read buffer for the plain-text scan; lives here rather than on the stack.
  // +4 for the incomplete UTF-8 sequence carried over from the previous chunk.
  char chunk[CHUNK_SIZE + 4] = {};

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
        ipaUtf8.append(seqBytes, seqLen);
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
      utf8.append(seqBytes, seqLen);
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
  // The screen this replaces was painted FAST and is very likely carrying the "Looking up"
  // toast: shouldShowPopup() now fires for every SD-font definition, and nothing repaints the
  // panel between that popup and render()'s displayBuffer below — so the toast box sits under
  // the incoming definition text unless this paint collapses the panel state first. Same
  // hazard EpubReaderActivity::drawIndexingPopup() works around (:490-493), and it applies
  // equally to the word-select highlight or a previous definition we may be replacing instead.
  // Costs one HALF refresh in place of a FAST on the first paint only.
  renderer.forceCleanRefreshNextPaint();
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
  LookupHistory::addWordIf(cachePath, historyWord, historyStatus, recordHistory);

  // Seed the back-nav chain. The initial word is the newest history entry iff it
  // was just logged (same condition addWordIf applies internally, including the
  // stopword filter — a filtered word is NOT recorded, so it has no history slot).
  chain_.reset(SETTINGS.getLookupHistoryCapValue());
  const bool initialLogged =
      recordHistory && !historyWord.empty() && !cachePath.empty() && !DictStopwords::isStopword(historyWord);
  chain_.setCurrentHistIndex(initialLogged ? 0 : -1);
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
  // Keeps the reader-size font and any CJK UI fallback target (see unloadExtraSizes()), so
  // this cannot strip the home/settings screens. Safe for the backgrounded word-select
  // activity too: that one measures and renders through getReaderFontId() exclusively.
  sdFontSystem.releaseExtraSizes(renderer);
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
  // A constant, deliberately NOT SETTINGS.getReaderScreenMargin(): a definition is dense
  // reference text read in short bursts, so screen width is worth more here than the breathing
  // room a book page wants, and a reader configured for 40px margins should not squeeze it.
  int bezelTop, bezelRight, bezelBottom, bezelLeft;
  renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
  constexpr int kScreenMargin = 5;

  // The landscape hint gutters stay inside these, so the side button hints are never overrun.
  contentX = bezelLeft + kScreenMargin + (isLandscapeCw ? hintGutterWidth : 0);
  leftPadding = contentX;
  rightPadding = bezelRight + kScreenMargin + (isLandscapeCcw ? hintGutterWidth : 0);
  contentTop = hintGutterHeight + bezelTop + kScreenMargin + metrics.topPadding;
  bodyStartY = contentTop + metrics.headerHeight + metrics.verticalSpacing;

  // Button hints are theme-owned chrome drawn at the panel edge on every screen in the app, so
  // they are not inset here; the margin instead keeps the last body line off them and off the
  // bottom bezel.
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing + bezelBottom + kScreenMargin;

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
  // Built-in body fonts decompress into a RAM cache on first use, so they never pay
  // per-glyph SD I/O; the scan below would be pure overhead for them. This whole path —
  // including the IPA prewarm — exists for the SD-font heap profile.
  if (!renderer.isSdCardFont(defFontId_)) return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;

  // From here the SD path starts pessimistic and has to earn ipaWarm_ back at the gate below.
  // Every early return past this point (collector OOM, dictionary file unreadable) is itself
  // evidence of a starved heap, which is exactly when the IPA font's per-glyph 11KB group
  // inflation drops glyphs — so those paths should land on the body-font fallback too.
  ipaWarm_ = false;

  const unsigned long t0 = millis();
  auto collector = makeUniqueNoThrow<PrewarmCollector>();
  if (!collector) {
    LOG_ERR("DDA", "OOM: prewarm collector (%u bytes)", static_cast<unsigned>(sizeof(PrewarmCollector)));
    return;  // not fatal: glyphs still load on demand, just slowly
  }
  collector->utf8.reserve(512);

  const std::string dictPath = foundLocation.folderPath + ".dict";
  if (defIsMarkup_) {
    // Same streaming producer the wrap uses, with a collecting sink instead of the
    // measuring Wrapper — so the styles seen here are exactly the styles drawn later.
    const DictHtmlRenderer::SpanSink sink{collector.get(), &DictionaryDefinitionActivity::collectSpanForPrewarm};
    htmlRenderer_.renderFromFileStreaming(dictPath.c_str(), foundLocation.offset, foundLocation.size, sink);
  } else {
    HalFile dictFile;
    if (!Storage.openFileForRead("DICT", dictPath.c_str(), dictFile)) return;
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
  // The gate tests the largest free BLOCK as well as total free heap, because what it is
  // authorising is a single contiguous malloc of group.uncompressedSize (11131 bytes for the
  // IPA font, FontDecompressor.cpp:487). On this activity's heap the two numbers diverge
  // badly — device logs routinely show free=15492 largest=9716 and free=9964 largest=4084 —
  // so a free-total gate happily passes an allocation that cannot succeed, and the failure
  // then resurfaces at draw time as dropped glyphs. Same rule as the body-style gates below
  // and as FontCacheManager.cpp:93-98.
  constexpr size_t kIpaGroupReserve = 12 * 1024;  // one FontDecompressor group, with margin
  const char* ipaOutcome = "none";
  if (!collector->ipaUtf8.empty()) {
    const size_t freeHeap = ESP.getFreeHeap();
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (freeHeap >= kIpaGroupReserve + kMinFreeForFirstStyle && largestBlock >= kIpaGroupReserve) {
      // A prewarm that reports missed groups leaves those glyphs on the same failing
      // hot-group path, so only a clean 0 counts as warm.
      const int missed = fcm->prewarmCache(IPA_FONT_ID, collector->ipaUtf8.c_str(), 0x01);
      ipaWarm_ = (missed == 0);
      ipaOutcome = ipaWarm_ ? "ok" : "miss";
    } else {
      ipaOutcome = "skip";
      LOG_DBG("DDA", "prewarm: skipping IPA (%u cp), free %u largest %u vs %u needed", collector->ipaCount,
              static_cast<unsigned>(freeHeap), static_cast<unsigned>(largestBlock),
              static_cast<unsigned>(kIpaGroupReserve));
    }
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
  if (!collector->utf8.empty()) {
    renderer.ensureSdCardFontReady(defFontId_, collector->utf8.c_str(), collector->styleMask);
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
  if (!collector->utf8.empty()) {
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
      fcm->prewarmCache(defFontId_, collector->utf8.c_str(), bit);
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
  // is fetched individually downstream. ipaWarm= is the phonetics equivalent, and unlike
  // warmed= it is a correctness signal rather than a speed one: anything but "ok" means the
  // IPA segments are being drawn in the body font (see ipaFontId()).
  SdDebugLog::log("DDA",
                  "prewarm sd=%d body=%u ipa=%u ipaWarm=%s mask=0x%02X warmed=0x%02X adv=%lums body=%lums free=%u "
                  "largest=%u",
                  renderer.isSdCardFont(defFontId_) ? 1 : 0, collector->uniqueCount, collector->ipaCount, ipaOutcome,
                  collector->styleMask, warmedMask, tAdv - tIpa, millis() - tAdv,
                  static_cast<unsigned>(ESP.getFreeHeap()),
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
  ipaRuns.clear();
  splitIpaRuns(text, ipaRuns);
  return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum + renderer.getTextWidth(run.isIpa ? ipaFontId() : defFontId_, run.text.c_str(), style);
  });
}

// ---------------------------------------------------------------------------
// HTML path: run DictHtmlRenderer, lay out spans into LayoutLines
// ---------------------------------------------------------------------------

int DictionaryDefinitionActivity::measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style,
                                                      bool isIpa) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int fontId = isIpa ? self->ipaFontId() : self->defFontId_;
  if (!isIpa && text[0] == ' ' && text[1] == '\0') return self->renderer.getSpaceWidth(fontId, style);
  return self->renderer.getTextWidth(fontId, text, style);
}

void DictionaryDefinitionActivity::wrapHtml() {
  const int maxWidth = renderer.getScreenWidth() - leftPadding - rightPadding;
  // Indent step: 3 spaces worth of pixels at regular weight.
  const int indentStep = renderer.getTextWidth(defFontId_, "   ");
  const int bulletWidth = renderer.getTextWidth(defFontId_, kBullet);

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
  const std::string dictPath = foundLocation.folderPath + ".dict";
  const DictHtmlRenderer::SpanSink spanSink{&wrapper, &DictionaryDefinitionActivity::feedSpanToWrapper};
  htmlRenderer_.renderFromFileStreaming(dictPath.c_str(), foundLocation.offset, foundLocation.size, spanSink);
  wrapper.finish();
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
    splitIpaRuns(currentLineText.c_str(), ipaRuns);
    // Sized exactly, so the push_back below cannot reallocate. Same reasoning as the sink:
    // an unguarded growth here aborts under -fno-exceptions, and this producer runs one
    // frame above collectLineSink — guarding only the sink would leave the reboot in place.
    if (!reserveNoThrow(line.segments, ipaRuns.size())) {
      collectOom_ = true;
    } else {
      for (const auto& run : ipaRuns) {
        line.segments.push_back({run.text, EpdFontFamily::REGULAR, run.isIpa});
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
  const std::string dictPath = foundLocation.folderPath + ".dict";
  HalFile dictFile;
  if (!Storage.openFileForRead("DICT", dictPath.c_str(), dictFile)) return;
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

  const int indentStep = renderer.getTextWidth(defFontId_, "   ");

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
      x += renderer.getTextWidth(defFontId_, kBullet);
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

        const int tokVisualWidth = renderer.getTextWidth(segFontId, tok.c_str(), seg.style);
        const int tokAdvanceX = renderer.getTextAdvanceX(segFontId, tok.c_str(), seg.style);
        std::string cleaned = Dictionary::cleanWord(tok);
        if (!cleaned.empty()) {
          uint16_t tokOff = WordSelectNavigator::poolAppend(textPool, tok.c_str(), tok.size());
          uint16_t cleanedOff = WordSelectNavigator::poolAppend(textPool, cleaned.c_str(), cleaned.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = tokOff;
          wi.textLen = static_cast<uint16_t>(tok.size());
          wi.lookupOffset = cleanedOff;
          wi.lookupLen = static_cast<uint16_t>(cleaned.size());
          wi.screenX = static_cast<int16_t>(x);
          wi.screenY = lineY;
          wi.width = static_cast<int16_t>(tokVisualWidth);
          wi.style = seg.style;
          wi.isIpa = seg.isIpa;
          wi.fontId = segFontId;
          words.push_back(wi);
        }
        x += tokAdvanceX;
      }
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
  LOG_DBG("DDA", "extractWords: %u words in %lums", static_cast<unsigned>(words.size()), millis() - t0);
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

void DictionaryDefinitionActivity::loop() {
  // --- Controller active (LookingUp / AltFormPrompt / NotFound) ---
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        const bool wasBackNav = chainBackNavInProgress;
        // Must match addWordIf exactly (incl. stopword filter) so the chain's
        // back-nav indices stay in lockstep with what actually lands in history.
        const bool willLog =
            !wasBackNav && controller.getRecordHistory() && !DictStopwords::isStopword(controller.getLookupWord());
        if (!wasBackNav) {
          // Forward: push a back-entry for the word being left (current headword,
          // on currentPage), referencing its history position.
          chain_.onForward(static_cast<uint16_t>(currentPage), willLog);
        }
        chainBackNavInProgress = false;
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
        // Chain-forward records; chain-back-nav does not.
        LookupHistory::addWordIf(cachePath, controller.getLookupWord(),
                                 DictionaryLookupController::toHistStatus(controller.getFoundStatus()), willLog);
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
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
    if (navigator.handleNavigation(mappedInput, renderer)) {
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
  const int indentStep = renderer.getTextWidth(defFontId_, "   ");

  // Header
  // Width spans between the two insets rather than the full screen, so the header band carries
  // the same margin as the body. In portrait both insets are equal, which keeps the band
  // symmetric about the screen-centred clock BaseTheme::drawTopBarClockDate draws into it.
  const char* headerText = headword.c_str();
#if LOG_LEVEL >= 2
  // Diagnostic: prefix the searched word with what opening this definition cost. Pinned to the
  // open (see openStartMs_), so paging up/down does not overwrite it with a page turn's ~0.5 s.
  // Blank on the first page and unavoidably so — the panel refresh below IS the dominant term
  // (device: display=3196ms of a 3439ms total), so any figure drawn before it would exclude
  // what is being measured, and repainting after it costs another full-panel refresh.
  //
  // Stack buffer, not std::string: this is a render path. No tr() — a bare "3.5s" carries no
  // language. Dev builds only; release is LOG_LEVEL=1 and drops the whole thing.
  char headerBuf[96];
  if (openMs_ > 0) {
    snprintf(headerBuf, sizeof(headerBuf), "%lu.%lus %s", openMs_ / 1000, (openMs_ % 1000) / 100, headword.c_str());
    headerText = headerBuf;
  }
#endif
  GUI.drawHeader(renderer,
                 Rect{contentX, contentTop, renderer.getScreenWidth() - contentX - rightPadding, metrics.headerHeight},
                 headerText);

  // Body: draw layout lines for the current page (BW pass). layoutLines holds
  // only the current page (Stage 2a streaming), so it is indexed from 0.
  const int lineHeight = getLineHeight();  // cached for loop + renderHighlight
  auto renderBody = [&]() {
    for (int i = 0; i < linesPerPage && i < static_cast<int>(layoutLines.size()); i++) {
      const PooledLine& line = layoutLines[i];
      const int y = bodyStartY + i * lineHeight;
      int x = leftPadding + line.indentLevel * indentStep;

      if (line.isListItem) {
        renderer.drawText(defFontId_, x, y, kBullet);
        x += renderer.getTextWidth(defFontId_, kBullet);
      }

      for (const auto& seg : line.segments) {
        const int segFontId = seg.isIpa ? ipaFontId() : defFontId_;
        const char* segText = pagePool_.data() + seg.offset;
        renderer.drawText(segFontId, x, y, segText, true, seg.style);
        if ((seg.style & EpdFontFamily::UNDERLINE) != 0) {
          const int segWidth = renderer.getTextWidth(segFontId, segText, seg.style);
          const int underlineY = y + renderer.getFontAscenderSize(segFontId) + 2;
          renderer.drawLine(x, underlineY, x + segWidth, underlineY, true);
        }
        x += renderer.getTextAdvanceX(segFontId, segText, seg.style);
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
  if (totalPages > 1) {
    char pageInfo[16];
    snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage + 1, totalPages);
    int textWidth = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
    renderer.drawText(SMALL_FONT_ID, renderer.getScreenWidth() - rightPadding - textWidth,
                      renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing, pageInfo);
  }

  const char* btn2 = showLookupButton ? tr(STR_LOOKUP_SHORT) : "";
  const char* btn3 = totalPages > 1 ? tr(STR_DIR_UP) : "";
  const char* btn4 = totalPages > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), btn2, btn3, btn4);
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
  if (openMs_ == 0) openMs_ = millis() - openStartMs_;
  logDictPhase(renderer, defFontId_, "render", millis() - t0, tDisplay - tBody, millis() - tDisplay);
}
