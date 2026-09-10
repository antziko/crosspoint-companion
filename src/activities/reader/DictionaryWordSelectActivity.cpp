#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <SdCardFont.h>  // getStats() on the reader font, for the render instrumentation
#include <SdDebugLog.h>
#include <Utf8.h>
#include <esp_heap_caps.h>  // heap_caps_get_largest_free_block: pre-flight for the word-array reserve
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/DictionaryRegistry.h"
#include "util/FlashcardDeck.h"
#include "util/PageTokenScan.h"

// Per-move SD trace for the gloss peek. On for the first device pass — measuring what a probe
// actually costs on the card is the whole point of it — and off once the numbers are in, at
// which point the serial LOG_DBG line remains. This is the ONLY per-keypress SD write on the
// gloss path; nothing else about it touches the card except reading the dictionary.
#ifndef DICT_GLOSS_TRACE
#define DICT_GLOSS_TRACE 1
#endif

namespace {

// Gloss box chrome, in pixels: 1 px frame, then padding before the text. Deliberately tight —
// the box costs page rows, so every pixel of chrome is a pixel of definition not shown.
constexpr int kGlossFrame = 1;
constexpr int kGlossPad = 4;
// Below this the box cannot hold a useful row, so the feature declines instead.
constexpr int kGlossMinWidth = 80;
// Vertical clearance the box keeps from the highlight: 7 px covers HighlightSnapshot's outward
// byte-alignment (WordSelectNavigator.h:215-231), plus 3 because the box is anchored to the row's
// y and a word may sit up to 2 px off it (the row-grouping tolerance in
// WordSelectNavigator.cpp:41).
constexpr int kGlossClearance = 10;

// Enlarged-character columns. The glyph is the definition font's own, drawn with each pixel
// replicated cellScale times (GfxRenderer::drawGlyphScaled) — a larger font is not an option
// here, since SdCardFontSystem::ensureFontSize snaps every request to a size the family ships
// (SdCardFontSystem.cpp:223-233) and would cost a third resident size besides.
//
// 3x against a 16pt definition font is a ~108 px character, which is the whole height of a
// three-row box. Past that the cell starves the definition on every screen this runs on.
constexpr int kGlossMaxCellScale = 3;
constexpr int kGlossCellGap = 8;
// What the text column must keep for the box to still be worth reading, whatever the cells want.
// Portrait is 480 logical px wide, so two 3x cells would leave ~12 Latin characters per row; the
// fraction is what pushes that case down to 2x while landscape keeps 3x, with no orientation
// branch anywhere. The absolute floor covers the smallest box the geometry gates allow.
constexpr int kGlossTextWidthNum = 3;
constexpr int kGlossTextWidthDen = 5;
constexpr int kGlossMinTextWidth = 120;

// The token as a single CJK codepoint, or 0 when it is anything else — a Latin word, a multi-
// character token, punctuation. Only a single character can go in a cell: CJK layout tokenises
// per character (see extractWords below), so this is the normal case on a Chinese page, while an
// English token is a whole word and keeps the flowed layout.
uint32_t singleCjkCodepoint(const char* text) {
  if (text == nullptr || *text == '\0') return 0;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  const uint32_t cp = utf8NextCodepoint(&cursor);
  if (*cursor != '\0') return 0;  // more than one codepoint
  if (!utf8IsCjkBreakable(cp) || utf8IsCjkPunctuation(cp)) return 0;
  return cp;
}

// Screen-space vertical extent of one page element, used to decide which elements have to be
// re-rendered when the box moves off a strip. A PageLine is one line of text (TextBlock.h:12) and
// carries no height at all, and even an element that does know its height can draw outside it —
// ruby annotations sit above the line, descenders below. So every extent is padded by one reader
// line either side. Erring wide only repaints pixels that are already correct; erring narrow
// leaves a white gap where the box used to be.
void elementExtent(const PageElement& el, int marginTop, int pad, int& outTop, int& outBottom) {
  const int top = el.yPos + marginTop;
  int bottom = top;
  if (el.getTag() == TAG_PageImage) {
    bottom = top + static_cast<const PageImage&>(el).getImageBlock().getHeight();
  }
  outTop = top - pad;
  outBottom = bottom + pad;
}

// A display token ends a sentence if its last byte is ASCII '.', '!' or '?'.
// Multibyte UTF-8 characters end with a continuation byte (>= 0x80), so a raw
// last-byte test never false-matches them.
bool endsSentence(const char* s) {
  if (!s || !*s) return false;
  const size_t n = strlen(s);
  const char c = s[n - 1];
  return c == '.' || c == '!' || c == '?';
}

// Single-style prewarm/advance-table bitmask: bit 0 = REGULAR, 1 = BOLD,
// 2 = ITALIC, 3 = BOLD_ITALIC. The `& 0x03` is defensive — Style enum
// is two bits, but UNDERLINE etc. live in higher bits if ever OR'd in.
constexpr uint8_t styleToBitMask(EpdFontFamily::Style style) {
  return static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
}

// Cached answer to initGloss()'s "is this dictionary plain-text?" probe.
//
// The probe reads the dictionary's .ifo off the SD card, and device instrumentation measured it
// at 331ms — a third of the 1226ms it costs to open word-select. It ran on EVERY entry because
// the activity, and therefore any member cache, is destroyed when the screen closes; the comment
// at the call site said "once per session", but a session is one entry. For a markup dictionary
// (the common case, where the answer is "no") that is 331ms spent re-learning the same fact
// every time the user picks a word.
//
// File scope so it outlives the activity, keyed by path so switching dictionaries re-probes.
// A fixed buffer rather than std::string: it holds at most one path and this is the heap-poorest
// screen in the app. Paths are built into char[128] elsewhere (buildDictPath), so the width
// matches; a path that does not fit is simply never cached rather than risking a prefix collision.
char g_plainProbePath[128] = "";
bool g_plainProbeIsPlain = false;

// Returns true when `path` has a cached answer, writing it to `out`.
bool plainDictProbeCached(const char* path, bool& out) {
  if (g_plainProbePath[0] == '\0' || strcmp(g_plainProbePath, path) != 0) return false;
  out = g_plainProbeIsPlain;
  return true;
}

void rememberPlainDictProbe(const char* path, bool isPlain) {
  if (strlen(path) >= sizeof(g_plainProbePath)) return;  // would truncate: skip rather than alias
  snprintf(g_plainProbePath, sizeof(g_plainProbePath), "%s", path);
  g_plainProbeIsPlain = isPlain;
}

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  // Instrumentation, not behaviour. This screen was the only stage of a lookup with no timing
  // line at all, which is why the ~2s between `MEM: enter DictionaryWordSelect` and
  // `DICT: lookup` could not be split into code time and the user finding their word. onEnter
  // and the first render() are the code half; whatever is left over is the user.
  const unsigned long tEnter0 = millis();
  // Remember the dictionary in force on the way in, before anything can install a per-word
  // one, so every fallback below and onExit() put back exactly this. See enterSessionDict_.
  enterSessionDict_ = Dictionary::sessionDictPath();
  enterSessionDictWasPromotion_ = Dictionary::sessionPathIsFallbackPromotion();
  // Quote selection looks nothing up, so it never needs the per-word dictionary.
  if (mode_ == Mode::Dictionary) controller.setPreLookupHook(&preLookupTrampoline, this);
  std::vector<WordSelectNavigator::WordInfo> words;
  std::vector<WordSelectNavigator::Row> rows;
  std::string textPool;  // sized exactly inside extractWords, from its counting pass
  extractWords(words, rows, textPool);
  const unsigned long tExtract = millis();
  mergeHyphenatedWords(words, rows, textPool);
  const unsigned long tMerge = millis();
  // Captured before load() moves the vector out. The navigator exposes only isEmpty(), and a
  // diagnostic is not a reason to widen its API.
  const unsigned wordCount = static_cast<unsigned>(words.size());
  // Only consume the initial Confirm release if Confirm is still held at onEnter — i.e.
  // we were opened mid hold-to-lookup. Other entry paths (e.g. reader menu → Lookup) have
  // already released Confirm by the time we open, so consuming would swallow the user's
  // first deliberate tap and force them to press twice.
  const bool consumeInitialConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  // Book text is all one font; no IPA runs here, so the second slot defaults to the first.
  navigator.setFonts(SETTINGS.getReaderFontId());
  // A quote is saved verbatim, so highlight selection has to reach "the" and "of" like any
  // other word; a lookup never wants them, so there the cursor walks past them.
  navigator.setSkipStopwords(mode_ == Mode::Dictionary);
  navigator.load(std::move(words), std::move(rows), std::move(textPool), consumeInitialConfirm, initialMarker_);
  // Opened by pointing at a word (the reader's page long-press): start on that word rather
  // than on initialMarker_'s band. In HighlightRange it is also the range anchor, so the
  // user's next tap picks the other end and finishes. A point that hits no word (margin,
  // inter-word gap) leaves the band selection as it is.
  if (initialPointX_ >= 0 && initialPointY_ >= 0) {
    const int hit =
        navigator.wordIndexAtPoint(initialPointX_, initialPointY_, renderer.getLineHeight(SETTINGS.getReaderFontId()));
    if (hit >= 0) {
      if (mode_ == Mode::HighlightRange) {
        navigator.beginMultiSelectAt(hit);
      } else {
        navigator.selectFlatIndex(hit);
        autoLookupPending_ = true;
      }
    }
  }
  const unsigned long tLoad = millis();
  // Opened via the reader's hold-Back gesture? Back is still held — swallow its release once.
  consumeInitialBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
  // After the word array and its text pool, never before: they are the big contiguous
  // requests, and the gloss is the optional extra.
  initGloss();
  // words= is the page's selectable-token count, which is what extract/merge/load all scale
  // with — without it a slow entry cannot be told from a dense page.
  SdDebugLog::log("DWS", "enter extract=%lums merge=%lums load=%lums gloss=%lums total=%lums words=%u free=%u",
                  tExtract - tEnter0, tMerge - tExtract, tLoad - tMerge, millis() - tLoad, millis() - tEnter0,
                  wordCount, static_cast<unsigned>(ESP.getFreeHeap()));
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  controller.onExit();
  // Hand back the dictionary that was in force on entry, dropping any per-word one applied
  // here. After controller.onExit(), deliberately: that stops and joins the lookup task, which
  // is the condition setSessionDictPath requires (Dictionary.h:98-99).
  DictUtils::restoreSessionDict(enterSessionDict_, enterSessionDictWasPromotion_);
  // Hand the box's refresh residue to the screen that replaces us, where the collapse is hidden
  // inside a screen change instead of interrupting a scan.
  clearGlossGhostOnNextPaint();
  // The session's three dictionary handles (.idx, page index, .dict) live inside GlossState, so
  // this is their intended release point — DESTRUCTOR_CLOSES_FILE only covers locals at scope
  // exit, and HalFile's destructor takes the storage mutex before closing.
  gloss_.reset();
  // Definition-size font, released HERE rather than in DictionaryDefinitionActivity::onExit (see
  // its note). This screen outlives the definition viewer and its gloss box draws in that font,
  // so releasing it there meant re-loading it on every re-entry — 620ms steady state, 904ms
  // cold, about half the press-to-highlight wait. Releasing at this point keeps the reader
  // underneath free of the extra size, which is the invariant that regression was about.
  DictUtils::releaseDefinitionFont(renderer);
  Activity::onExit();
}

void DictionaryWordSelectActivity::prewarmHighlightGlyphs(int currIdx) {
  const auto* w = navigator.getWordAt(currIdx);
  if (!w) return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  fcm->prewarmCache(SETTINGS.getReaderFontId(), navigator.getDisplay(*w), styleToBitMask(w->style));
}

void DictionaryWordSelectActivity::prebuildAdvanceTable() {
  // Concatenate every word on the page and OR the style flags. ~2KB transient
  // string; freed on return. Matches the per-font scan buffers
  // FontCacheManager::PrewarmScope accumulates into.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t pageStyleMask = 0;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const uint16_t n = block->wordCount();
    for (uint16_t i = 0; i < n; i++) {
      pageText.append(block->wordText(i), block->wordTextLen(i));
      pageText.push_back(' ');
      pageStyleMask |= styleToBitMask(block->wordStyle(i));
    }
  }
  if (pageStyleMask == 0) pageStyleMask = styleToBitMask(EpdFontFamily::REGULAR);
  // The advance table persists across clearCache() (SdCardFont.h:201) so
  // this only pays the SD cost on the first entry; subsequent ones
  // amortize.
  renderer.ensureSdCardFontReady(SETTINGS.getReaderFontId(), pageText.c_str(), pageStyleMask);
}

void DictionaryWordSelectActivity::countTokens(size_t& outWords, size_t& outPoolBytes, size_t& outRows) const {
  outWords = 0;
  outPoolBytes = 0;
  outRows = 0;
  int16_t lastY = 0;
  bool haveRow = false;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const uint16_t n = block->wordCount();
    for (uint16_t i = 0; i < n; i++) {
      const char* text = block->wordText(i);
      const size_t len = block->wordTextLen(i);
      bool isCjk = false;
      if (!PageTokens::isSelectable(text, len, isCjk)) continue;

      // Upper bound: a token with d dashes yields at most d + 1 parts, and their bytes sum
      // to at most len (the dash bytes themselves are dropped). Exact for the overwhelming
      // majority — the existing code notes dash-split words run ~0-2 per page — and erring
      // high only costs a few spare entries in a reservation, never a re-allocation.
      const size_t parts = PageTokens::countDashes(text, len) + 1;
      outWords += parts;
      outPoolBytes += len + parts;  // one NUL terminator per part

      // Mirrors organizeIntoRows' 2px Y tolerance so the row reserve is right too.
      const int16_t y = static_cast<int16_t>(line->yPos);
      if (!haveRow || std::abs(y - lastY) > 2) {
        outRows++;
        lastY = y;
        haveRow = true;
      }
    }
  }
}

void DictionaryWordSelectActivity::extractWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                std::vector<WordSelectNavigator::Row>& rows, std::string& textPool) {
  words.clear();
  rows.clear();

  // Count first, then make exactly one allocation each. The flat word array has to land in
  // ONE contiguous block, and CJK layout tokenises per *character* (ParsedText.cpp:399-423),
  // so a Chinese page produces ~400 entries where English produces ~60. Letting the vector
  // reach that by doubling asks for 8192 bytes at the 256-entry step — and std::vector's
  // growth routes to a THROWING operator new, which abort()s under -fno-exceptions instead
  // of returning null. That is the reported crash: 8192 wanted against a 7412-byte largest
  // free block, from a Chinese book's dictionary lookup.
  size_t wantWords = 0, wantPoolBytes = 0, wantRows = 0;
  countTokens(wantWords, wantPoolBytes, wantRows);

  if (wantWords > 0) {
    // Probe the block before reserving. reserve() itself cannot fail safely here, so the
    // check has to happen first — the same idiom, for the same reason, as
    // FontCacheManager.cpp:105-109 and KOReaderSyncClient.cpp:528-537. Headroom covers the
    // rows array, the pool, and the per-token std::string temporaries below.
    constexpr size_t kExtractHeadroom = 4096;
    const size_t wordBytes = wantWords * sizeof(WordSelectNavigator::WordInfo);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < wordBytes + kExtractHeadroom) {
      // Degrade rather than abort: onEnter() loads an empty navigator, render() draws the
      // empty state and loop() lets Back out (see the navigator.isEmpty() branch below).
      LOG_ERR("DWS", "Word list too large for heap: need %u+%u, largest %u - selection disabled", (unsigned)wordBytes,
              (unsigned)kExtractHeadroom, (unsigned)largest);
      return;
    }
    words.reserve(wantWords);
    rows.reserve(wantRows);
    // Reserve the pool exactly too, so TextPool::append's +256 linear growth — which also
    // routes to the aborting operator new — never fires. The slack absorbs the merged
    // lookup strings mergeHyphenatedWords appends afterwards (at most one per row).
    constexpr size_t kPoolSlack = 256;
    textPool.reserve(wantPoolBytes + kPoolSlack);
  }

  // Populate the SD font's advance table once so every getTextAdvanceX call
  // below takes the fast in-RAM path.
  prebuildAdvanceTable();

  // Fallback used by blocks where we can't derive a per-line gap
  // (single-word blocks, degenerate first-word measurements).
  const int16_t naturalSpaceWidth =
      static_cast<int16_t>(renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), " ", EpdFontFamily::REGULAR));

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;

    // Flat per-word storage (TextBlock stores words back-to-back in a single
    // NUL-terminated arena; wordText(i) is a stable const char*, wordTextLen(i)
    // its byte length excluding the NUL).
    const uint16_t blockWordCount = block->wordCount();

    // Per-line gap = xPos[1] - xPos[0] - firstWordWidth. Justified blocks
    // stretch the gap (ParsedText.cpp:514-553 adds justifyExtra), so a
    // global space-width can't be reused — we measure per-block.
    int16_t lineGapWidth = naturalSpaceWidth;
    if (blockWordCount >= 2 && block->wordTextLen(0) > 0) {
      const EpdFontFamily::Style firstStyle = block->wordStyle(0);
      const std::string firstWord(block->wordText(0), block->wordTextLen(0));
      const int16_t firstWidth =
          PageTokens::measureAdvance(renderer, SETTINGS.getReaderFontId(), firstWord, firstStyle);
      const int16_t derivedGap = static_cast<int16_t>(block->wordXpos(1) - block->wordXpos(0) - firstWidth);
      // When wordList[1] is a continuation (attached punctuation etc., ParsedText.cpp:537-544)
      // the layout inserts no inter-word gap, so derivedGap collapses to the kerning offset
      // (~1-3 px). Real gaps are always >= getSpaceAdvance(...), so a half-space threshold
      // cleanly separates a real gap from a continuation kerning without needing Block to
      // expose continuesVec. Without the threshold, an undersized lineGapWidth propagates as
      // a per-word width overestimate (~4-6 px) — the highlight rectangle bleeds past the
      // word into the inter-word space.
      if (derivedGap > naturalSpaceWidth / 2) lineGapWidth = derivedGap;
    }

    // Ruby-annotated lines shift their base text down by half an ascender (see
    // TextBlock::getRubyShift). Move the tap targets in lockstep so the selection
    // boxes stay aligned with the rendered word positions.
    const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(SETTINGS.getReaderFontId()));

    for (uint16_t wIdx = 0; wIdx < blockWordCount; wIdx++) {
      int16_t screenX = line->xPos + block->wordXpos(wIdx) + marginLeft;
      int16_t screenY = line->yPos + marginTop + rubyShift;
      const EpdFontFamily::Style wordStyle = block->wordStyle(wIdx);

      // Skip tokens with no letter or digit (bullets, punctuation, etc.).
      // This was a bare std::isalnum byte scan, which rejects every byte >= 0x80 and so
      // dropped wholly non-ASCII tokens ("中", "漢字") — leaving CJK pages with no
      // selectable words at all and no cursor. The CJK clause is strictly additive: a
      // Latin token carries no CJK codepoint, so its verdict — and therefore every
      // word's flat index, which the bookmark store persists as a highlight anchor —
      // is unchanged. Cf. Dictionary.cpp:22, the same byte-vs-codepoint fix on the
      // lookup side.
      //
      // Tested before the std::string is built, so rejected tokens cost no allocation —
      // and via the same helper countTokens() used, so the reserve above cannot drift
      // out of step with what actually gets pushed.
      bool isCjkToken = false;
      if (!PageTokens::isSelectable(block->wordText(wIdx), block->wordTextLen(wIdx), isCjkToken)) {
        continue;
      }
      // Page composition, reported in the gloss timing lines: CJK pages are the expensive case
      // (one token per character, so the most cursor stops and the coldest glyphs), and the
      // numbers are unreadable without knowing which kind of page produced them. Free here —
      // the tokeniser has just decided both facts — where a separate pass would re-walk every
      // token's codepoints.
      selectableTokenCount_++;
      if (isCjkToken) cjkTokenCount_++;
      const std::string wordText(block->wordText(wIdx), block->wordTextLen(wIdx));

      // En/em-dash split. The rule lives in PageTokenScan.h because the reader's quote
      // underline has to resolve stored word indices against this same numbering. A stack
      // array, not a vector: this runs per token, and the reserve(4) it replaces was one heap
      // block per word on the page.
      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t partCount =
          PageTokens::collectParts(wordText.data(), wordText.size(), parts, PageTokens::kMaxTokenParts);
      const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == wordText.size();

      if (unsplit) {
        // width = (xPos[i+1] - xPos[i]) - lineGapWidth, which is the layout's
        // xpos diff with the trailing inter-word gap removed. Punctuation
        // tokens skipped above kept their xpos entries as boundary markers,
        // so this works regardless of what the next token is.
        // Last word per block has no next xpos; fall back to direct
        // measurement. Clamp to 1 to guard pathological cases (continuation
        // negative kerning, short words where the entire xpos diff is the
        // gap).
        int16_t wordWidth;
        if (isCjkToken) {
          // The xpos-diff derivation below assumes an inter-word gap that CJK does not
          // have: layout adds nothing to totalNaturalGaps for a noSpaceBefore token
          // (ParsedText.cpp:1207-1210), so lineGapWidth falls back to a full space width
          // and the highlight box comes out a space too narrow. A justified CJK line
          // additionally carries a variable justifyExtra in that diff, which must not
          // land inside the box either. Measuring is exact and costs nothing here — CJK
          // tokens are single characters and prebuildAdvanceTable() already made this an
          // in-RAM advance-table hit.
          wordWidth = PageTokens::measureAdvance(renderer, SETTINGS.getReaderFontId(), wordText, wordStyle);
        } else if (wIdx + 1 < blockWordCount) {
          const int16_t raw = static_cast<int16_t>(block->wordXpos(wIdx + 1) - block->wordXpos(wIdx));
          wordWidth = std::max(static_cast<int16_t>(1), static_cast<int16_t>(raw - lineGapWidth));
        } else {
          wordWidth = PageTokens::measureAdvance(renderer, SETTINGS.getReaderFontId(), wordText, wordStyle);
        }
        {
          uint16_t off = WordSelectNavigator::poolAppend(textPool, wordText.c_str(), wordText.size());
          WordSelectNavigator::WordInfo wi;
          wi.textOffset = off;
          wi.textLen = static_cast<uint16_t>(wordText.size());
          wi.lookupOffset = off;
          wi.lookupLen = wi.textLen;
          wi.screenX = screenX;
          wi.screenY = screenY;
          wi.width = wordWidth;
          wi.style = wordStyle;
          words.push_back(wi);
        }
      } else {
        for (size_t si = 0; si < partCount; si++) {
          const size_t start = parts[si].start;
          std::string part = wordText.substr(start, parts[si].end - start);
          std::string prefix = wordText.substr(0, start);
          // Dash-split words are rare (~0-2 per page); per-part measurement
          // is fine here. Soft-hyphen stripping matches the rest of
          // extractWords and matches layout's preprocessor.
          int16_t offsetX =
              prefix.empty() ? 0 : PageTokens::measureAdvance(renderer, SETTINGS.getReaderFontId(), prefix, wordStyle);
          int16_t partWidth = PageTokens::measureAdvance(renderer, SETTINGS.getReaderFontId(), part, wordStyle);
          {
            uint16_t off = WordSelectNavigator::poolAppend(textPool, part.c_str(), part.size());
            WordSelectNavigator::WordInfo wi;
            wi.textOffset = off;
            wi.textLen = static_cast<uint16_t>(part.size());
            wi.lookupOffset = off;
            wi.lookupLen = wi.textLen;
            wi.screenX = static_cast<int16_t>(screenX + offsetX);
            wi.screenY = screenY;
            wi.width = partWidth;
            wi.style = wordStyle;
            words.push_back(wi);
          }
        }
      }
    }
  }

  WordSelectNavigator::organizeIntoRows(words, rows);
}

void DictionaryWordSelectActivity::mergeHyphenatedWords(std::vector<WordSelectNavigator::WordInfo>& words,
                                                        std::vector<WordSelectNavigator::Row>& rows,
                                                        std::string& textPool) {
  WordSelectNavigator::mergeHyphenatedPairs(words, rows, textPool);

  // Cross-page hyphenation: update lookup text when the last word on this page
  // ends with a hyphen and its continuation begins the next page.
  if (!nextPageFirstWord.empty() && !rows.empty()) {
    int lastWordIdx = rows.back().firstWord + rows.back().wordCount - 1;
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen > 0 && utf8EndsWithHyphen(lastWord, lastLen) && lastWord[0] != '-') {
      std::string firstPart(lastWord, lastLen);
      utf8RemoveTrailingHyphen(firstPart);
      std::string merged = firstPart + nextPageFirstWord;
      uint16_t off = WordSelectNavigator::poolAppend(textPool, merged.c_str(), merged.size());
      words[lastWordIdx].lookupOffset = off;
      words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    }
  }

  rows.erase(
      std::remove_if(rows.begin(), rows.end(), [](const WordSelectNavigator::Row& r) { return r.wordCount == 0; }),
      rows.end());
}

// Page-local sentence the current selection sits in, for the flashcard front
// face. Walks outward from the selected word (or the anchor..cursor span for a
// phrase) to the nearest sentence-ending token or page edge, bounded by word
// count, then windows that span around the selection to fit the deck's excerpt
// cap. Page-clipped sentences are accepted (the navigator only holds the current
// page). Returns "" if there is no selection.
std::string DictionaryWordSelectActivity::buildLookupExcerpt() const {
  const int sel = navigator.getCurrentFlatIndex();
  if (sel < 0) return "";

  // Phrase lookups span anchor..cursor; single lookups are just the cursor word.
  const int anchor = navigator.getAnchorFlatIndex();
  int lo = sel, hi = sel;
  if (anchor >= 0) {
    lo = std::min(anchor, sel);
    hi = std::max(anchor, sel);
  }

  static constexpr int MAX_EXCERPT_WORDS = 40;  // bounds the joined string length
  // Sentence bounds around the selection. Each side is scanned independently: sharing one
  // budget let the left side spend all of it and starve the right, which is how the
  // looked-up word ended up outside its own excerpt.
  int sLo = lo, sHi = hi;
  // Extend left until the previous token ends a sentence (or page start).
  while (sLo > 0 && (lo - sLo) < MAX_EXCERPT_WORDS) {
    const auto* prev = navigator.getWordAt(sLo - 1);
    if (!prev || endsSentence(navigator.getDisplay(*prev))) break;
    sLo--;
  }
  // Extend right until the current token ends a sentence (or page end).
  while ((sHi - hi) < MAX_EXCERPT_WORDS) {
    const auto* cur = navigator.getWordAt(sHi);
    if (!cur || endsSentence(navigator.getDisplay(*cur))) break;
    if (!navigator.getWordAt(sHi + 1)) break;  // page edge
    sHi++;
  }

  // Window the sentence around the selection rather than trimming its tail: the card face
  // underlines the word by finding it in the excerpt, so the word must survive the cap.
  std::string excerpt = navigator.buildPhraseWindow(sLo, sHi, lo, hi, FlashcardDeck::EXCERPT_MAX, MAX_EXCERPT_WORDS);
  // Backstop for the one case the window cannot fit: a multi-select phrase whose own text
  // is over the cap. Trim on a word boundary where possible (the deck also hard-caps, but
  // this avoids storing a mid-word fragment).
  if (static_cast<int>(excerpt.size()) > FlashcardDeck::EXCERPT_MAX) {
    excerpt.resize(FlashcardDeck::EXCERPT_MAX);
    const size_t sp = excerpt.find_last_of(' ');
    if (sp != std::string::npos && sp > 0) {
      excerpt.resize(sp);
    } else {
      // CJK excerpts have no spaces, so there is no word boundary to fall back to and
      // the resize above can land mid-sequence. Trim to the last complete codepoint —
      // a partial sequence renders as a broken glyph on the flashcard face.
      excerpt.resize(static_cast<size_t>(utf8SafeTruncateBuffer(excerpt.data(), static_cast<int>(excerpt.size()))));
    }
  }
  return excerpt;
}

void DictionaryWordSelectActivity::preLookupTrampoline(void* ctx, const std::string& cleanedWord) {
  static_cast<DictionaryWordSelectActivity*>(ctx)->applyCardDictForLookup(cleanedWord);
}

void DictionaryWordSelectActivity::applyCardDictForLookup(const std::string& cleanedWord) {
  if (cachePath.empty()) return;
  const unsigned long t0 = millis();
  uint32_t recorded = 0;
  // Keyed on the CLEANED word because that is what enroll() files the card under (see the
  // FoundDefinition case below, which passes controller.getLookupWord()).
  const bool hasCard = FlashcardDeck::cardDict(cachePath, cleanedWord, recorded);
  // recorded != 0 short-circuits before applyCardDict so the common no-card case does not log
  // a "-> none" line on every lookup. A card naming a dictionary that is NOT installed here
  // still reaches applyCardDict and still logs, because that is the case worth diagnosing.
  const bool applied = hasCard && recorded != 0 && DictUtils::applyCardDict(recorded);
  if (!applied) {
    // No card, a card recording nothing (legacy line, or a peer that sent no 'D' line), or one
    // naming a dictionary this device does not have -> the screen's entry dictionary, NOT a
    // blanket clear. Restoring is also what stops the previous word's dictionary bleeding onto
    // this one: the hook runs before EVERY lookup, so each starts from a known state.
    DictUtils::restoreSessionDict(enterSessionDict_, enterSessionDictWasPromotion_);
  }
  // This probe adds an SD pass to every lookup (FlashcardDeck::forEachLine reads in 64-byte
  // chunks, each taking the storage mutex), and it is a SECOND read of the same field —
  // DictionaryDefinitionActivity::onEnter reads it again. Logged so the cost is measured
  // against DICT: lookup rather than assumed.
  SdDebugLog::log("DWS", "card dict probe: ms=%lu card=%d hash=%lu applied=%d", millis() - t0, hasCard ? 1 : 0,
                  static_cast<unsigned long>(recorded), applied ? 1 : 0);
}

void DictionaryWordSelectActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        // Auto-enroll the looked-up word as a flashcard. This is the sole site
        // with live page context for the excerpt; the navigator still holds the
        // page words + current selection here (it is reset on activity exit).
        if (!cachePath.empty()) {
          // Record WHICH dictionary answered, so the card's back face is later rendered from
          // that one rather than whatever happens to be active at review time — the whole point
          // being that a word saved from a Chinese dictionary must not flip to an English
          // definition after a switch. activeDictPath() resolves the session override too, so a
          // dictionary chosen BEFORE this lookup is captured, not just the configured selection.
          // A dictionary chosen AFTER it — in the definition screen's select mode — is written
          // over this value there, by FlashcardDeck::setCardDict.
          // The last two arguments are the re-count window: the same word looked up again
          // within it leaves its card untouched (no count bump, no deck rewrite). This is the
          // only enroll call site with a clock, and the only one that passes a window.
          FlashcardDeck::enroll(cachePath, controller.getLookupWord(), buildLookupExcerpt(), chapterTitle_,
                                DictUtils::activeDictHash(cachePath.c_str()), millis(),
                                static_cast<uint32_t>(SETTINGS.flashcardRecountMins) * 60000UL);
        }
        // Nothrow because this push runs on the most stressed heap in the firmware: the popup
        // render's glyph prewarm has just taken its arena, and this object is ~4.8 KB. A bare
        // new here aborts the device (it did — DictionaryWordSelectActivity.cpp:506 in the X3
        // crash trace); failing back to the word list is a far better outcome than a reboot.
        // The definition screen repaints everything anyway, so let its first paint also collapse
        // the residue the box left in its band.
        clearGlossGhostOnNextPaint();
        auto definition = makeUniqueNoThrow<DictionaryDefinitionActivity>(
            renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(), true, cachePath,
            controller.getRecordHistory(), controller.getLookupWord(),
            DictionaryLookupController::toHistStatus(controller.getFoundStatus()),
            // The only entry point that may delete the card: this is the lookup that created it,
            // and the reader page behind us is where its underline is drawn.
            /*allowCardDelete=*/true);
        if (!definition) {
          LOG_ERR("DWS", "OOM: DictionaryDefinitionActivity");
          forceFullRepaintOnNextRender();
          requestUpdate();
          break;
        }
        startActivityForResult(std::move(definition), [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            setResult(ActivityResult{});
            finish();
          } else {
            forceFullRepaintOnNextRender();
            requestUpdate();
          }
        });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        forceFullRepaintOnNextRender();
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        forceFullRepaintOnNextRender();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  // Swallow the Back release that ended the launching hold-Back gesture (see onEnter), so it
  // doesn't fall through to the cancel handlers below on the first tick.
  if (consumeInitialBackRelease_) {
    const bool released = mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
      consumeInitialBackRelease_ = false;
      if (released) return;
    }
  }

  if (navigator.isEmpty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  // Pointed at a word and asked for a lookup in one gesture: run it. Fired here rather than
  // from onEnter so it starts on the loop task, the same task every other lookup starts on,
  // and after the controller gate above -- which means the controller owns the frame from
  // this tick, and its overlay is what renders, so the word-select screen never flashes past.
  if (autoLookupPending_) {
    autoLookupPending_ = false;
    controller.lookupSelected(navigator);
    return;
  }

  if (navigator.handleNavigation(mappedInput, renderer, SETTINGS.getReaderSwapWordSelectAxes())) {
    requestUpdate();
  }

  // Touch: a touch-down walks the cursor to the word under the finger (differential
  // repaint, the same path a button step takes), and a tap on a word acts on it. On a
  // board with no Confirm button this is the ONLY way to select anything.
  //
  // Dictionary mode stops at the multi-select boundary: there the two ends are walked with
  // the cursor and a stray tap would silently move the anchor's far end. HighlightRange
  // instead RANGES by tapping -- first tap anchors, second tap ends and saves -- because a
  // touch-only board has no Confirm to long-press into multi-select with.
  const bool tapRanging = mode_ == Mode::HighlightRange;
  if (mappedInput.hasTouch() && (tapRanging || !navigator.isMultiSelecting())) {
    const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
    int tx = 0;
    int ty = 0;
    // Only before a range is open. renderHighlightDifferential declines in multi-select
    // (WordSelectNavigator.cpp), so a touch-down preview there would cost a full page
    // repaint for pixels the tap that follows is about to replace anyway.
    if (!navigator.isMultiSelecting() && mappedInput.wasScreenTouchDown(tx, ty)) {
      if (navigator.selectFlatIndex(navigator.wordIndexAtPoint(tx, ty, lineHeight))) requestUpdate();
      return;
    }
    if (mappedInput.wasScreenTapped(tx, ty)) {
      const int hit = navigator.wordIndexAtPoint(tx, ty, lineHeight);
      if (hit >= 0) {
        if (mode_ == Mode::Dictionary) {
          navigator.selectFlatIndex(hit);
          controller.lookupSelected(navigator);
          return;
        }
        if (navigator.isMultiSelecting()) {
          // Second tap: the range is [anchor, this word]. Tapping the anchor again is a
          // legitimate one-word highlight, so no minimum length is enforced.
          const int anchor = navigator.getAnchorFlatIndex();
          navigator.selectFlatIndex(hit);
          emitQuoteResult(anchor, hit, navigator.buildPhrase(anchor, hit));
          return;
        }
        // First tap: anchor here and show it highlighted while the user picks the end.
        navigator.beginMultiSelectAt(hit);
        requestUpdate();
        return;
      }
    }
  }

  // Check Back early when not in multi-select mode. This allows exit even when
  // confirmReleaseConsumed is stuck true (menu-triggered entry has no Confirm release).
  if (!navigator.isMultiSelecting() && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  // HighlightRange mode: reuse the same single-word + long-press-range selection gesture,
  // but emit a quote result instead of a dictionary lookup.
  if (mode_ == Mode::HighlightRange) {
    handleHighlightInput();
    return;
  }

  if (controller.handleMultiSelect(navigator)) return;

  if (navigator.isMultiSelecting()) return;

  if (controller.handleConfirmLookup(navigator)) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

const char* DictionaryWordSelectActivity::confirmHintLabel() const {
  return mode_ == Mode::HighlightRange ? tr(STR_ADD_HIGHLIGHT) : tr(STR_LOOKUP_SHORT);
}

void DictionaryWordSelectActivity::handleHighlightInput() {
  std::string phrase;
  const auto act = navigator.handleMultiSelectInput(mappedInput, phrase);
  if (act != WordSelectNavigator::MultiSelectAction::None) {
    if (act == WordSelectNavigator::MultiSelectAction::PhraseReady) {
      emitQuoteResult(navigator.getAnchorFlatIndex(), navigator.getCurrentFlatIndex(), std::move(phrase));
    } else {
      // Entered/Exited multi-select, or a consumed long-press carryover — just repaint
      // so the highlight reflects the new selection state.
      requestUpdate();
    }
    return;
  }

  // A plain Confirm tap (no long-press, not in multi-select) saves a single-word quote.
  if (!navigator.isMultiSelecting() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto* sel = navigator.getSelected();
    if (sel) {
      const int idx = navigator.getCurrentFlatIndex();
      emitQuoteResult(idx, idx, navigator.getDisplay(*sel));
    }
  }
}

void DictionaryWordSelectActivity::emitQuoteResult(int fromFlatIdx, int toFlatIdx, std::string previewText) {
  if (fromFlatIdx < 0 || toFlatIdx < 0) {
    DictUtils::cancelAndFinish(*this);  // selection went stale; treat as cancel
    return;
  }
  HighlightRangeResult result;
  result.startWordIndex = std::min(fromFlatIdx, toFlatIdx);
  result.endWordIndex = std::max(fromFlatIdx, toFlatIdx);
  result.previewText = std::move(previewText);
  setResult(ActivityResult{std::move(result)});
  finish();
}

// ---------------------------------------------------------------------------
// Inline gloss box
// ---------------------------------------------------------------------------

int DictionaryWordSelectActivity::measureGlossWidth(void* ctx, const char* text, EpdFontFamily::Style style, bool) {
  auto* self = static_cast<DictionaryWordSelectActivity*>(ctx);
  // Only ever called from inside DictGloss::fit, which only runs with a gloss allocated.
  return self->renderer.getTextAdvanceX(self->gloss_->fontId, text, style);
}

void DictionaryWordSelectActivity::resolveGlossFont() {
  if (!gloss_) return;

  // "Same as book" follows the reader's FAMILY at the dictionary's own SIZE, and getDefinitionFontId
  // can only resolve to that size once the family is resident at it — the resolver allocates
  // nothing itself (CrossPointSettings.cpp:495-499). Same call the definition viewer makes
  // (DictionaryDefinitionActivity.cpp:257); it declines itself when the size is already resident,
  // when the family ships no such size, or when the heap is too tight, and we then render at the
  // reader's size exactly as the definition screen does.
  if (SETTINGS.dictionaryFontFamily == CrossPointSettings::DICT_FONT_MATCH_READER) {
    sdFontSystem.ensureFontSize(SETTINGS.getReaderSdFontFamilyName(), SETTINGS.getDefinitionPointSize(), renderer);
  }

  // Re-resolved on every peek rather than cached once, because the id can go stale underneath
  // us: DictionaryDefinitionActivity::onExit releases the extra size (its :321), on the
  // documented assumption that word-select renders through getReaderFontId() only — which stopped
  // being true when the box moved to the definition font. So after Confirm → definition → Back,
  // this is what re-establishes the size and picks up whatever id is actually live now.
  const int fontId = SETTINGS.getDefinitionFontId();
  if (fontId == gloss_->fontId) return;

  gloss_->fontId = fontId;
  gloss_->lineHeight = renderer.getLineHeight(fontId);
  gloss_->height = DictGloss::GlossResult::kMaxRows * gloss_->lineHeight + 2 * (kGlossPad + kGlossFrame);
  // Height budget for an enlarged cell: the rows it spans, over the font's own ascender, which is
  // what a Han glyph fills. Width is settled per token in planGlossCells.
  gloss_->ascender = renderer.getFontAscenderSize(fontId);
  const int rowsHeight = DictGloss::GlossResult::kMaxRows * gloss_->lineHeight;
  gloss_->maxCellScale = gloss_->ascender > 0 ? std::min(kGlossMaxCellScale, rowsHeight / gloss_->ascender) : 0;
  // A dictionary size well above the reader's can make the box too tall for the page to be worth
  // reading around; when it is, the box is simply not drawn (see drawGloss). The worst case is
  // the box plus its clearance plus the selected row and one more row of context still visible —
  // which is what minTextRoom holds. Per-selection placement is stricter still (placeGloss).
  gloss_->fits = gloss_->bottomLimit - gloss_->topY >= gloss_->height + kGlossClearance + gloss_->minTextRoom;
  // Metrics changed, so the rows wrapped at the old ones are wrong. Forcing a re-peek is cheaper
  // to reason about than re-wrapping from a buffer fit() has already compacted in place.
  gloss_->forFlatIdx = -1;
  LOG_DBG("DGL", "font=%d lh=%d h=%d fits=%d", fontId, gloss_->lineHeight, gloss_->height, gloss_->fits ? 1 : 0);
}

void DictionaryWordSelectActivity::planGlossCells(const char* token, const char* entry) {
  const int innerW = gloss_->width - 2 * (kGlossFrame + kGlossPad);

  // Flowed layout is the default and the fallback for every decline below: the whole box is text,
  // exactly as it was before the columns existed.
  gloss_->tokenCp = 0;
  gloss_->variantCp = 0;
  gloss_->dropField = false;
  gloss_->cellScale = 0;
  gloss_->cellCount = 0;
  gloss_->cellW = 0;
  gloss_->textX = gloss_->x + kGlossFrame + kGlossPad;
  gloss_->wrapWidth = innerW;

  // Scale 1 is the character at its ordinary size in a column of its own, which is worse than no
  // column at all — so a box too short to double it declines outright.
  if (gloss_->maxCellScale < 2) return;
  const uint32_t cp = singleCjkCodepoint(token);
  if (cp == 0) return;

  // Measured off the advance table, which the prewarm has already made resident for this token —
  // no glyph load, so this costs nothing on the per-keypress path.
  const int advance = renderer.getTextAdvanceX(gloss_->fontId, token, EpdFontFamily::REGULAR);
  if (advance <= 0) return;

  // The entry's own headword field — the bytes before the reading — is the script variant. Worth a
  // column only when it is a single character that differs from the one on the page; when
  // traditional and simplified coincide, which is most characters, a second cell would just show
  // the same glyph twice. Anything longer stays in the flowed text (DictGloss::FitOptions).
  uint32_t fieldCp = 0;
  if (entry != nullptr) {
    const DictGloss::ReadingSpan reading = DictGloss::findReading(entry);
    size_t len = reading.found ? reading.start : 0;
    while (len > 0 && static_cast<unsigned char>(entry[len - 1]) <= 0x20) len--;
    char field[16];
    if (len > 0 && len < sizeof(field)) {
      memcpy(field, entry, len);
      field[len] = '\0';
      fieldCp = singleCjkCodepoint(field);
    }
  }
  const uint32_t variantCp = fieldCp != cp ? fieldCp : 0;
  // A field that merely repeats the character on the page carries nothing, so it comes out of the
  // text as well as staying out of a cell. A field this could not read as a single character — a
  // multi-character or Latin headword — is left in the text, where it is the only place it would
  // appear at all.
  const bool fieldIsToken = fieldCp != 0 && fieldCp == cp;

  // Widest arrangement that still leaves a definition worth reading, preferring to keep both
  // characters over keeping them large — a variant shown small beats a variant not shown.
  for (int cells = variantCp != 0 ? 2 : 1; cells >= 1; cells--) {
    for (int scale = gloss_->maxCellScale; scale >= 2; scale--) {
      const int cellW = advance * scale;
      const int used = cells * (cellW + kGlossCellGap);
      const int textW = innerW - used;
      if (textW < kGlossMinTextWidth) continue;
      if (textW * kGlossTextWidthDen < innerW * kGlossTextWidthNum) continue;

      gloss_->tokenCp = cp;
      gloss_->variantCp = cells > 1 ? variantCp : 0;
      gloss_->dropField = cells > 1 || fieldIsToken;
      gloss_->cellScale = scale;
      gloss_->cellCount = cells;
      gloss_->cellW = cellW;
      gloss_->textX = gloss_->x + kGlossFrame + kGlossPad + used;
      gloss_->wrapWidth = textW;
      return;
    }
  }
}

void DictionaryWordSelectActivity::initGloss() {
  if (mode_ != Mode::Dictionary) return;  // quote selection looks nothing up
  if (!SETTINGS.dictInlineGlossEnabled) return;
  if (navigator.isEmpty()) return;

  // Gate: plain-text dictionaries only. A folder named "st-" is that group by convention
  // (DictionaryRegistry.h:49); a declared sametypesequence of 'm' says the same thing, and is
  // checked second because the .ifo field is optional and often absent or wrong — which is why
  // the folder-name rule replaced it for grouping in the first place. Markup dictionaries need
  // DictHtmlRenderer's expat arena (~6.9 KB retained, DictHtmlRenderer.h:47-56) on the heap this
  // screen has least of, so they stay out until that headroom is measured.
  //
  // Note the book's script is deliberately NOT part of this gate: a CJK page read with a markup
  // dictionary still could not be rendered here, so the script decides nothing on its own.
  const std::string dictPath = Dictionary::activeDictPath(cachePath.c_str());
  if (dictPath.empty()) return;
  const int regIdx = dictionaryRegistry.indexOf(dictPath);
  bool plainDict = regIdx >= 0 && dictionaryRegistry.getEntries()[regIdx].nameIsSt;
  if (!plainDict && !plainDictProbeCached(dictPath.c_str(), plainDict)) {
    // 608 bytes, so heap rather than the 256-byte stack budget (Dictionary.h:130-138). Once per
    // session, never per cursor move.
    auto info = makeUniqueNoThrow<DictInfo>();
    if (!info) {
      LOG_ERR("DGL", "OOM: DictInfo");
      return;
    }
    if (!Dictionary::readInfoInto(dictPath.c_str(), *info)) return;  // unreadable: do not cache
    plainDict = info->sametypesequence[0] == 'm';
    rememberPlainDictProbe(dictPath.c_str(), plainDict);
  }
  if (!plainDict) {
    LOG_DBG("DGL", "off: not a plain-text dictionary");
    return;
  }

  // Heap floor. First estimates, same standing as the other gates on this screen
  // (DictionaryDefinitionActivity.cpp:388): GlossState is ~1.2 KB and the peek allocates only
  // the transient strings DictLayout's wrapper makes. Tune from the logged values.
  constexpr size_t kMinFreeForGloss = 8 * 1024;
  constexpr size_t kMinBlockForGloss = 4 * 1024;
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < kMinFreeForGloss || largestBlock < kMinBlockForGloss) {
    LOG_ERR("DGL", "off: free=%u largest=%u (need %u/%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(largestBlock), static_cast<unsigned>(kMinFreeForGloss),
            static_cast<unsigned>(kMinBlockForGloss));
    return;
  }

  // Geometry. Orientation-aware exactly as the definition viewer composes it
  // (DictionaryDefinitionActivity.cpp:347-385): the panel's physical viewable area first, then
  // the reader's own margin, then the button-hint chrome — which is a bottom band in Portrait,
  // also a top band when Inverted, and a side column in both Landscapes. Nothing here assumes
  // a screen size: X3 (792x528) and X4 (800x480) share one binary.
  //
  // Only the horizontal extents and the vertical limits the box may not cross are settled here.
  // Everything that depends on the box's own line height belongs to resolveGlossFont(), because
  // that height follows the definition font and the definition font can change mid-session.
  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
  int bezelTop, bezelRight, bezelBottom, bezelLeft;
  renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orient = renderer.getOrientation();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const int sideGutter = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int hintBand = metrics.buttonHintsHeight + metrics.verticalSpacing;

  const int x = marginLeft + (isLandscapeCw ? sideGutter : 0);
  const int rightInset = bezelRight + SETTINGS.getReaderScreenMargin() + (isLandscapeCcw ? sideGutter : 0);
  const int width = renderer.getScreenWidth() - x - rightInset;
  const int topY = std::max(marginTop, bezelTop + (isInverted ? hintBand : 0));
  const int bottomLimit = renderer.getScreenHeight() - bezelBottom - hintBand;

  if (width < kGlossMinWidth) {
    LOG_DBG("DGL", "off: no room (w=%d)", width);
    return;
  }

  auto state = makeUniqueNoThrow<GlossState>();
  if (!state) {
    LOG_ERR("DGL", "OOM: GlossState (%u bytes)", static_cast<unsigned>(sizeof(GlossState)));
    return;
  }

  // One ctx for the whole session: .idx and the page index stay open, so a cursor move costs
  // one seek+scan instead of the four SD opens a fresh locate() pays (Dictionary.h:140-155).
  if (!Dictionary::openLookupCtx(state->ctx, cachePath.c_str())) {
    LOG_DBG("DGL", "off: dictionary unreadable");
    return;
  }
  // Without a page index, locate() degrades to a full .idx scan. On this synchronous
  // per-keypress path that is a watchdog risk, not just slow, so decline instead.
  if (!state->ctx.hasPageIndex) {
    LOG_DBG("DGL", "off: no page index (.idx.oft/.cspt) — run Prepare on this dictionary");
    return;
  }
  char dictFilePath[160];
  snprintf(dictFilePath, sizeof(dictFilePath), "%s.dict", state->ctx.base);
  if (!Storage.openFileForRead("DGL", dictFilePath, state->dict)) {
    LOG_DBG("DGL", "off: %s will not open", dictFilePath);
    return;
  }

  state->x = x;
  state->width = width;
  state->topY = topY;
  state->bottomLimit = bottomLimit;
  // Two rows of the reader's own text, since that is what the box is measured against covering.
  state->minTextRoom = 2 * lineHeight;

  gloss_ = std::move(state);
  resolveGlossFont();

  // A page that cannot show the box plus two rows of text outside it gets no box at all, rather
  // than one covering everything there is to read.
  if (!gloss_->fits) {
    LOG_DBG("DGL", "off: no room (h=%d page=%d)", gloss_->height, bottomLimit - topY);
    gloss_.reset();
    return;
  }

  // The miss label is constant, so warm it once here instead of on the first miss — where it
  // would land in the middle of a frame the user is waiting on.
  constexpr uint8_t kRegularOnly = styleToBitMask(EpdFontFamily::REGULAR);
  renderer.ensureSdCardFontReady(gloss_->fontId, tr(STR_DICT_NOT_FOUND), kRegularOnly);
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->prewarmCache(gloss_->fontId, tr(STR_DICT_NOT_FOUND), kRegularOnly);
  }

  LOG_DBG("DGL", "on: w=%d h=%d top=%d bottom=%d cjk=%d/%d", width, gloss_->height, topY, bottomLimit, cjkTokenCount_,
          selectableTokenCount_);
}

bool DictionaryWordSelectActivity::updateGloss(int currIdx, int selectionLineHeight) {
  if (!gloss_) return false;
  if (currIdx < 0 || controller.isActive()) {
    gloss_->place = false;
    return false;
  }
  const auto* w = navigator.getWordAt(currIdx);
  if (!w) {
    gloss_->place = false;
    return false;
  }

  // Before the peek: it decides the wrap width's line count and can force a re-peek.
  resolveGlossFont();

  // Placement. Directly above the selected row by preference — that covers rows already read —
  // and directly below when the selection sits too near the top for the box to fit above.
  // kGlossClearance is not cosmetic: HighlightSnapshot::capture snaps its rectangle outward to
  // byte boundaries along the panel-memory x-axis, which is the screen VERTICAL axis in Portrait,
  // so a box within ~7 px of the highlight can be captured with it and pasted back stale later
  // (WordSelectNavigator.h:215-231). Keeping the gap here makes that unreachable by construction.
  //
  // Multi-select is excluded outright: the highlight then spans a range of rows that the
  // single-token peek does not describe, and that the box could be sitting inside.
  //
  // Anchored to the ROW, not to the word: words are grouped into a row with a 2 px tolerance
  // (WordSelectNavigator.cpp:41), so anchoring to the word's own screenY would shift the box a
  // pixel or two on a plain left/right step — and every shift is a strip restore and a clean
  // refresh. Anchored to the row, a scan along one row leaves the box perfectly still.
  gloss_->place = false;
  if (gloss_->fits && !navigator.isMultiSelecting()) {
    const int rowTop = navigator.rowY(w->row);
    const int above = rowTop - kGlossClearance - gloss_->height;
    const int below = rowTop + selectionLineHeight + kGlossClearance;
    if (above >= gloss_->topY) {
      gloss_->y = above;
      gloss_->place = true;
    } else if (below + gloss_->height <= gloss_->bottomLimit) {
      gloss_->y = below;
      gloss_->place = true;
    }
  }

  if (currIdx != gloss_->forFlatIdx) {
    gloss_->forFlatIdx = currIdx;
    const unsigned long t0 = millis();
    // Through cleanWord, exactly as the Confirm path does (DictionaryLookupController's
    // lookupOrPopup). Skipping it would make the box disagree with the screen Confirm opens on
    // the very tokens cleanWord exists for — attached CJK punctuation, quotes, trailing commas —
    // reporting "not found" for a word the full lookup then finds. Short tokens stay inside the
    // std::string SSO buffer, so a CJK character or an ordinary word costs no allocation.
    const std::string token = Dictionary::cleanWord(navigator.getLookup(*w));
    const size_t n = token.empty() ? 0
                                   : DictGloss::readEntry(gloss_->ctx, gloss_->dict, token.c_str(), gloss_->raw,
                                                          sizeof(gloss_->raw));
    const unsigned long tProbe = millis();
    unsigned long tWarm = tProbe;

    // The selected character warmed on its own, on the hit path as well as the miss one. It is
    // drawn enlarged from this same font, and it is NOT reliably part of the entry below: a
    // simplified page against an entry whose headword field is the traditional form shares no
    // glyph at all. On a miss nothing else is warmed, so without this the enlarged character
    // would take SdCardFont's ~50 ms per-glyph path on the very frame that reports the miss.
    constexpr uint8_t kRegularOnly = styleToBitMask(EpdFontFamily::REGULAR);
    if (!token.empty()) {
      renderer.ensureSdCardFontReady(gloss_->fontId, token.c_str(), kRegularOnly);
      if (auto* fcm = renderer.getFontCacheManager()) fcm->prewarmCache(gloss_->fontId, token.c_str(), kRegularOnly);
    }

    if (n == 0) {
      gloss_->result.reset();
      // nullptr, not gloss_->raw: readEntry leaves the buffer holding the PREVIOUS word's entry.
      planGlossCells(token.c_str(), nullptr);
      tWarm = millis();
    } else {
      // Before any measuring, never after: a codepoint that misses here falls through to
      // SdCardFont's per-glyph path at ~50 ms each, and one CJK gloss is 30-60 glyphs. Same
      // ordering, and the same two calls, as prebuildAdvanceTable + prewarmHighlightGlyphs
      // above — advance table first (measurement), bitmaps second (drawing). Against the gloss
      // font, which is a different size from the page's, so none of the page's warm glyphs count.
      // This also covers the variant character, which is part of the entry by definition.
      renderer.ensureSdCardFontReady(gloss_->fontId, gloss_->raw, kRegularOnly);
      if (auto* fcm = renderer.getFontCacheManager()) fcm->prewarmCache(gloss_->fontId, gloss_->raw, kRegularOnly);

      // The reading renders bold, which is a separate glyph set. Warmed over the bracketed run
      // ALONE, never the whole entry: bold Han would double the glyph work on the one path that
      // runs per keypress, for characters that are never drawn bold. The run is ASCII and its
      // alphabet is tiny, so after the first few words it is permanently warm. Found on the raw
      // bytes, which is also where fit() decides whether the run is on the first line.
      const DictGloss::ReadingSpan reading = DictGloss::findReading(gloss_->raw);
      if (reading.found) {
        constexpr uint8_t kBoldOnly = styleToBitMask(EpdFontFamily::BOLD);
        char* const run = gloss_->raw + reading.start;
        const char saved = run[reading.len];
        run[reading.len] = '\0';
        renderer.ensureSdCardFontReady(gloss_->fontId, run, kBoldOnly);
        if (auto* fcm = renderer.getFontCacheManager()) fcm->prewarmCache(gloss_->fontId, run, kBoldOnly);
        run[reading.len] = saved;
      }
      tWarm = millis();

      // Before fit(), which compacts the buffer in place and moves every offset in it. The wrap
      // width it settles is what the entry is then wrapped to.
      planGlossCells(token.c_str(), gloss_->raw);

      const DictLayout::Measurer measure{this, &DictionaryWordSelectActivity::measureGlossWidth};
      const DictLayout::WrapMetrics wrapMetrics{gloss_->wrapWidth, 0, 0};
      // Columns only when there are cells: with the character enlarged beside it, the reading gets
      // a row of its own so the eye finds it in the same place on every move, and the variant is
      // dropped from the text exactly when it is being drawn as a cell instead.
      DictGloss::FitOptions opts;
      opts.readingOnOwnRow = gloss_->cellScale > 0;
      opts.dropLeadingField = gloss_->dropField;
      DictGloss::fit(gloss_->raw, wrapMetrics, measure, gloss_->result, opts);
    }
    const unsigned long tFit = millis();
    LOG_DBG("DGL", "peek '%s' %s rows=%d probe=%lums warm=%lums fit=%lums", token.c_str(),
            gloss_->result.found ? "hit" : "miss", gloss_->result.rowCount, tProbe - t0, tWarm - tProbe, tFit - tWarm);
#if DICT_GLOSS_TRACE
    // Per-move SD write, and the only one on this path — it is what phase 0 is for. Turn
    // DICT_GLOSS_TRACE off once the timings are known; the serial line above stays.
    //
    // row0 and bold= are here to answer a question no fixture in the repo can: what a real 'm'
    // entry actually looks like on the card. bold= is offset+length of the bracketed reading in
    // row 0, so a wrong split shows up as numbers that do not bracket "[...]" in row0.
    SdDebugLog::log(
        "DGL", "peek %s bytes=%u rows=%d bold=%u+%u probe=%lu warm=%lu fit=%lu cjk=%d/%d free=%u largest=%u row0='%s'",
        gloss_->result.found ? "hit" : "miss", static_cast<unsigned>(n), gloss_->result.rowCount,
        static_cast<unsigned>(gloss_->result.boldStart[0]), static_cast<unsigned>(gloss_->result.boldLen[0]),
        tProbe - t0, tWarm - tProbe, tFit - tWarm, cjkTokenCount_, selectableTokenCount_,
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
        gloss_->result.rowCount > 0 ? gloss_->result.rows[0] : "");
#endif
  }

  // A move matters only when the framebuffer actually holds a box somewhere else. Giving that
  // strip back to the page is normally a handful of text lines (restoreVacatedGlossStrip), which
  // keeps a row change on the differential path. An image in the strip is the one thing that
  // cannot be put back cheaply — it may need decoding — so that case alone falls to the full
  // repaint, which redraws the page anyway.
  const bool moving = gloss_->drawnY != kGlossNotDrawn && (!gloss_->place || gloss_->drawnY != gloss_->y);
  return moving && stripHasImage(gloss_->drawnY, gloss_->height);
}

void DictionaryWordSelectActivity::renderPageStrip(int y, int height) {
  renderer.clearRect(gloss_->x, y, gloss_->width, height);

  const int fontId = SETTINGS.getReaderFontId();
  const int pad = renderer.getLineHeight(fontId);

  // Scan pass then real pass, exactly as the full repaint below does. Not optional here: the gloss
  // prewarm loads the DEFINITION font's glyphs into the same cache, so the reader-font glyphs in
  // this strip may well have been evicted since the page was drawn. Every cold one would otherwise
  // fall to SdCardFont's per-glyph path at ~50 ms each; the scan pass collapses the whole strip
  // into one batched SD read. The scan pass draws nothing (GfxRenderer::isFontCacheScanning).
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    for (const auto& el : page->elements) {
      int top, bottom;
      elementExtent(*el, marginTop, pad, top, bottom);
      if (bottom <= y || top >= y + height) continue;
      el->render(renderer, fontId, marginLeft, marginTop);
    }
    scope.endScanAndPrewarm();
  }

  for (const auto& el : page->elements) {
    int top, bottom;
    elementExtent(*el, marginTop, pad, top, bottom);
    if (bottom <= y || top >= y + height) continue;
    el->render(renderer, fontId, marginLeft, marginTop);
  }
}

bool DictionaryWordSelectActivity::stripHasImage(int y, int height) const {
  const int pad = renderer.getLineHeight(SETTINGS.getReaderFontId());
  for (const auto& el : page->elements) {
    if (el->getTag() != TAG_PageImage) continue;
    int top, bottom;
    elementExtent(*el, marginTop, pad, top, bottom);
    if (bottom > y && top < y + height) return true;
  }
  return false;
}

bool DictionaryWordSelectActivity::restoreVacatedGlossStrip() {
  if (!gloss_ || gloss_->drawnY == kGlossNotDrawn) return false;
  if (gloss_->place && gloss_->y == gloss_->drawnY) return false;  // parked — the common case

  // The whole old rectangle, not just the part the new box will leave uncovered: the new box is
  // drawn on top straight afterwards, so re-rendering a line or two underneath it costs almost
  // nothing and removes a class of off-by-one from the overlap arithmetic.
  renderPageStrip(gloss_->drawnY, gloss_->height);
  gloss_->drawnY = kGlossNotDrawn;

  // The box has moved, so this frame already shows a visibly different layout — the one moment
  // where collapsing the accumulated FAST-refresh residue costs nothing perceptually. It is what
  // keeps a box that walks down the page from leaving a trail behind it, and it is deliberately
  // NOT the periodic refresh this replaced: that one fired while the box sat still, which read
  // as the screen flashing mid-scan. A left/right step along one row never reaches here.
  renderer.forceCleanRefreshNextPaint();
  return true;
}

// The box's rows advance by gloss_->lineHeight, which is the DEFINITION font's and is generally a
// different size from the reader's. Placement (updateGloss) has already decided where it goes and
// whether it goes anywhere at all.
void DictionaryWordSelectActivity::drawGloss() {
  // Nothing to show for this selection: no room on either side, multi-select, or no selection at
  // all. Whatever was on screen has already been dealt with — the differential path calls
  // restoreVacatedGlossStrip first, and the full-repaint path has just redrawn the page over it.
  if (!gloss_ || !gloss_->place) return;
  const int y = gloss_->y;

  renderer.clearRect(gloss_->x, y, gloss_->width, gloss_->height);
  renderer.drawRect(gloss_->x, y, gloss_->width, gloss_->height, true);

  const int textX = gloss_->textX;
  int textY = y + kGlossFrame + kGlossPad;

  // Enlarged character column(s), each closed by a rule so the eye reads them as cells rather
  // than as oversized text that happens to precede the definition. Centred on the rows they span:
  // the scale was chosen to fit that height, so it always leaves a little slack.
  if (gloss_->cellScale > 0 && gloss_->tokenCp != 0) {
    const int rowsHeight = DictGloss::GlossResult::kMaxRows * gloss_->lineHeight;
    const int glyphTop = textY + (rowsHeight - gloss_->ascender * gloss_->cellScale) / 2;
    int cellX = gloss_->x + kGlossFrame + kGlossPad;

    const uint32_t cells[2] = {gloss_->tokenCp, gloss_->variantCp};
    for (int i = 0; i < gloss_->cellCount; i++) {
      if (cells[i] == 0) break;
      // Drawn at the cell's left edge rather than centred in it: cellW is the token's own scaled
      // advance and Han advances are uniform, so the two glyphs line up without measuring the
      // second one.
      renderer.drawGlyphScaled(gloss_->fontId, cells[i], cellX, glyphTop, gloss_->cellScale, true);
      cellX += gloss_->cellW + kGlossCellGap;
      renderer.drawLine(cellX - kGlossCellGap / 2, y + kGlossFrame, cellX - kGlossCellGap / 2,
                        y + gloss_->height - kGlossFrame - 1, true);
    }
  }

  if (gloss_->result.found) {
    for (int i = 0; i < gloss_->result.rowCount; i++) {
      char* row = gloss_->result.rows[i];
      const uint8_t boldStart = gloss_->result.boldStart[i];
      const uint8_t boldLen = gloss_->result.boldLen[i];
      if (boldLen > 0) {
        // Head, reading, tail. drawText takes a NUL-terminated C string and one style, so each
        // piece is drawn with the terminator moved onto its end byte and put straight back — no
        // second buffer on a path that runs per cursor move. x advances by the measured width of
        // what was just drawn, so the row still reads as one continuous line.
        int x = textX;
        if (boldStart > 0) {
          const char saved = row[boldStart];
          row[boldStart] = '\0';
          renderer.drawText(gloss_->fontId, x, textY, row, true);
          x += renderer.getTextAdvanceX(gloss_->fontId, row, EpdFontFamily::REGULAR);
          row[boldStart] = saved;
        }
        char* const run = row + boldStart;
        const char saved = run[boldLen];
        run[boldLen] = '\0';
        renderer.drawText(gloss_->fontId, x, textY, run, true, EpdFontFamily::BOLD);
        x += renderer.getTextAdvanceX(gloss_->fontId, run, EpdFontFamily::BOLD);
        run[boldLen] = saved;
        renderer.drawText(gloss_->fontId, x, textY, run + boldLen, true);
      } else {
        renderer.drawText(gloss_->fontId, textX, textY, row, true);
      }
      textY += gloss_->lineHeight;
    }
  } else {
    // Say so rather than showing an empty frame: while scanning, "looked up, absent" is
    // information, and a blank box reads as a bug.
    renderer.drawText(gloss_->fontId, textX, textY, tr(STR_DICT_NOT_FOUND), true);
  }
  gloss_->drawnY = y;
}

void DictionaryWordSelectActivity::clearGlossGhostOnNextPaint() {
  if (!gloss_ || gloss_->drawnY == kGlossNotDrawn) return;
  // Repainting the same strip over and over with FAST refreshes leaves residue behind, and only a
  // state-collapsing refresh removes it. Two moments qualify: when the box relocates (handled in
  // restoreVacatedGlossStrip) and the transitions out of it — leaving word-select, and opening the
  // full definition — which is what this call covers. Both are screen changes, so the refresh is
  // invisible inside them, and the residue never outlives the box that caused it.
  renderer.forceCleanRefreshNextPaint();
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  const unsigned long tRender0 = millis();
  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
  const int currIdx = navigator.getCurrentFlatIndex();

  // Peek the selection and place its box before either repaint path runs. Placement is relative
  // to the selected row, so it changes on a row change, not on every keypress.
  const bool glossNeedsFullRepaint = updateGloss(currIdx, lineHeight);

  // Differential fast path. Only valid when:
  //   - we set it up on the previous frame (RenderMode::Differential),
  //   - the controller has nothing pending to draw,
  //   - we have a current selection,
  //   - the strip the gloss box is vacating can be re-rendered without the page (no image in it).
  if (nextRenderMode_ == RenderMode::Differential && !controller.isActive() && currIdx >= 0 && !glossNeedsFullRepaint) {
    prewarmHighlightGlyphs(currIdx);
    // Before the highlight, never after: this re-renders page lines, and one redrawn over a
    // freshly drawn highlight would print black text across the inverted rectangle. It is also
    // clear of the PREVIOUS highlight — the box kept kGlossClearance from the selection it was
    // placed against — so the snapshot the call below is about to restore stays untouched.
    const bool relocationScrubbed = restoreVacatedGlossStrip();
    auto dirty = navigator.renderHighlightDifferential(renderer, lineHeight, prevHighlightIdx_, currIdx);
    if (dirty.has_value()) {
      // Drawn after the highlight, never before: renderHighlightDifferential captures the pixels
      // under the new highlight first, and the box must not be inside that capture. Placement
      // keeps them kGlossClearance apart by construction, and the box region is fully overwritten
      // every time, so it needs no snapshot of its own.
      drawGloss();
      // Push full panel — the SDK's windowed-refresh path produces alternating black→white
      // transition failures on consecutive fast partial refreshes, so it's intentionally not
      // wired up here. The savings come from skipping page->render, which dominates the
      // pre-optimization cost; the full push at the end is a hardware floor (~444ms).
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      // One line per cursor move. This is the path that decides how long "the user finding
      // their word" really takes: every move ends in a full-panel push, so N moves to reach a
      // word cost N x this. If that product is most of the pre-lookup gap, the fix is here and
      // not in the entry render. scrub=1 marks a relocation frame, which re-renders the vacated
      // strip and collapses the box's residue — measured at roughly twice a plain move.
      SdDebugLog::log("DWS", "render diff total=%lums scrub=%d free=%u", millis() - tRender0, (int)relocationScrubbed,
                      static_cast<unsigned>(ESP.getFreeHeap()));
      prevHighlightIdx_ = currIdx;
      return;
    }
    // Fall through to full repaint.
  }

  // Skip-initial-render fast path. Fires at most once per activity instance,
  // when the caller signalled the framebuffer already contains the page at
  // our margins (currently only EpubReaderActivity's hold-to-lookup path).
  // Conditions:
  //   - flag still set (one-shot),
  //   - controller has nothing to draw (an active controller would mean we
  //     re-entered render() after a sub-activity returned without the
  //     framebuffer being reset by forceFullRepaintOnNextRender()),
  //   - we have a current selection (currIdx >= 0); otherwise there is
  //     nothing to overlay and we fall through to a normal repaint.
  // We consume the flag unconditionally on first entry so any later
  // full-repaint goes through the normal clearScreen + page->render path.
  if (framebufferContainsPage_) {
    framebufferContainsPage_ = false;
    if (!controller.isActive() && currIdx >= 0) {
      // Clear the bottom strip the caller reserved (status bar OR auto-turn
      // label). Match the menu→lookup path, which wipes via clearScreen() +
      // page->render(); we skipped both, so clear that one region instead.
      if (reservedBottomHeight_ > 0) {
        int bezelTop, bezelRight, bezelBottom, bezelLeft;
        renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
        const int clearY = renderer.getScreenHeight() - bezelBottom - reservedBottomHeight_;
        const int clearW = renderer.getScreenWidth() - bezelLeft - bezelRight;
        renderer.clearRect(bezelLeft, clearY, clearW, reservedBottomHeight_);
      }

      prewarmHighlightGlyphs(currIdx);

      auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
      bool snapshotPrimed = setup.has_value();
      if (!snapshotPrimed) {
        // Hyphenated wrap or oversize capture. The framebuffer still holds
        // the page, but we cannot prime the snapshot for the differential
        // path. Draw the multi-word highlight (which overwrites pixels under
        // each highlight rect) and force the next render to do a full
        // repaint so the renderer state is consistent. The user just pays
        // for one regular page render on the next cursor move instead of
        // on entry.
        navigator.renderHighlight(renderer, lineHeight);
      }
      drawGloss();
      const auto labels = mappedInput.mapLabels("", confirmHintLabel(), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      // The cheap entry: the caller left the page in the framebuffer, so this skips BOTH page
      // renders. Seeing this line instead of "render full" means entry cost is already near the
      // panel floor and the double render is not what the first lookup is paying for.
      SdDebugLog::log("DWS", "render skip-initial total=%lums free=%u", millis() - tRender0,
                      static_cast<unsigned>(ESP.getFreeHeap()));
      prevHighlightIdx_ = currIdx;
      nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
      return;
    }
    // Flag was set but conditions weren't met (controller active or no
    // current selection). Fall through to the normal full-repaint path.
  }

  // Full repaint path.
  const unsigned long tFull0 = millis();
  renderer.clearScreen();
  if (controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    if (gloss_) gloss_->drawnY = kGlossNotDrawn;
    return;
  }

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // Without this, every cold codepoint cold-misses the 8-slot SD glyph
  // overflow ring and the page render serializes ~100+ individual SD reads.
  // Same pattern as EpubReaderActivity::renderContents().
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);  // scan pass
  const unsigned long tScan = millis();
  scope.endScanAndPrewarm();
  const unsigned long tPrewarm = millis();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop);
  const unsigned long tDraw = millis();

  // Set up snapshot AND draw the highlight via the differential entry point with
  // prevWordIdx = -1 (no previous highlight to wipe). This both draws the highlight
  // for this frame and primes snapshot_ so the next frame can run the fast path.
  // If the navigator declines (multi-select, hyphenated, oversize), fall back to
  // the multi-word renderHighlight and stay on the full path next frame.
  //
  // The -1 literal is load-bearing: renderHighlightDifferential uses prevWordIdx
  // < 0 as the signal "framebuffer was just redrawn from scratch, discard any
  // stale snapshot rather than restoring it on top of fresh pixels." This is the
  // only path that disturbs the framebuffer outside the differential cycle, so
  // it's also the only call site that must pass -1.
  bool snapshotPrimed = false;
  if (currIdx >= 0) {
    auto setup = navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx);
    snapshotPrimed = setup.has_value();
  }
  if (!snapshotPrimed) {
    navigator.renderHighlight(renderer, lineHeight);
  }
  const unsigned long tHighlight = millis();

  // The page has just been redrawn, so wherever the box was is page text again — no strip to
  // give back, and this is the frame that resolves a move the differential path declined.
  if (gloss_) gloss_->drawnY = kGlossNotDrawn;
  drawGloss();
  const unsigned long tGloss = millis();

  const auto labels = mappedInput.mapLabels("", confirmHintLabel(), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  const unsigned long tDisplay = millis();

  // The page is rendered TWICE here — a scan pass to collect glyphs, then the real draw — so
  // scan= and draw= are logged apart: if they are both large the double render is the cost, if
  // only scan= is, the prewarm is not paying for itself. scan= also carries clearScreen() and
  // PrewarmScope's clearCache(), which is where the total starts. display= is the panel floor
  // (~437ms) and is not fixable from here.
  //
  // Miss counters need no reset: PrewarmScope's constructor calls resetStats()
  // (FontCacheManager.cpp:98), so these are scoped to this render. Same counters the DDA lines
  // read, so a word-select miss count is directly comparable to a definition one.
  uint32_t misses = 0, missMs = 0;
  const int readerFontId = SETTINGS.getReaderFontId();
  if (renderer.isSdCardFont(readerFontId)) {
    const auto& fonts = renderer.getSdCardFonts();
    const auto it = fonts.find(readerFontId);
    if (it != fonts.end() && it->second) {
      misses = it->second->getStats().overflowMisses;
      missMs = it->second->getStats().overflowMissMs;
    }
  }
  SdDebugLog::log("DWS",
                  "render full scan=%lums prewarm=%lums draw=%lums hl=%lums gloss=%lums display=%lums total=%lums "
                  "miss=%u missMs=%u free=%u largest=%u",
                  tScan - tFull0, tPrewarm - tScan, tDraw - tPrewarm, tHighlight - tDraw, tGloss - tHighlight,
                  tDisplay - tGloss, tDisplay - tFull0, misses, missMs, static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  prevHighlightIdx_ = currIdx;
  nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
}
