#include "DictionaryDefinitionActivity.h"

#include <DictHtmlRenderer.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
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

}  // namespace

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
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
  requestUpdate();
  // SD write overlaps the e-ink refresh kicked by requestUpdate() on the render task.
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
  controller.onExit();
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

  // Resolve the per-definition invariants once (see defFontId_ / defIsHtml_).
  defFontId_ = SETTINGS.getDefinitionFontId();
  const DictInfo info = Dictionary::readInfo(foundLocation.folderPath.c_str());
  defIsHtml_ = info.valid && info.sametypesequence[0] == 'h';

  const auto orient = renderer.getOrientation();
  const auto metrics = UITheme::getInstance().getMetrics();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  hintGutterHeight = isInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int sidePadding = metrics.contentSidePadding + SETTINGS.screenMargin;
  leftPadding = contentX + sidePadding;
  rightPadding = (isLandscapeCcw ? hintGutterWidth : 0) + sidePadding;
  bodyStartY = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const int topArea = hintGutterHeight + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;

  linesPerPage = (renderer.getScreenHeight() - topArea - bottomArea) / getLineHeight();
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("DDA", "wrapText: font=%d sd=%d dictFamily=%u html=%d linesPerPage=%d", defFontId_,
          renderer.isSdCardFont(defFontId_) ? 1 : 0, SETTINGS.dictionaryFontFamily, defIsHtml_ ? 1 : 0, linesPerPage);

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

  const unsigned long t0 = millis();
  auto collector = makeUniqueNoThrow<PrewarmCollector>();
  if (!collector) {
    LOG_ERR("DDA", "OOM: prewarm collector (%u bytes)", static_cast<unsigned>(sizeof(PrewarmCollector)));
    return;  // not fatal: glyphs still load on demand, just slowly
  }
  collector->utf8.reserve(512);

  const std::string dictPath = foundLocation.folderPath + ".dict";
  if (defIsHtml_) {
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

  // IPA before the body font, deliberately. IPA runs are drawn with a built-in font whose
  // non-prewarmed path decompresses a whole ~11KB group per glyph (FontDecompressor.cpp:182);
  // once the body prewarm below has taken its share, that contiguous block no longer exists
  // and every IPA glyph is silently skipped ("OOM hot group ... glyph skipped"). Prewarming
  // it first, while the heap is least fragmented, both fixes that and drops the per-glyph
  // decompress. The IPA family is single-style (main.cpp:113), so 0x01 covers every style
  // the segments are drawn in.
  if (!collector->ipaUtf8.empty()) {
    fcm->prewarmCache(IPA_FONT_ID, collector->ipaUtf8.c_str(), 0x01);
  }
  const unsigned long tIpa = millis();

  // Body font, one style at a time, most-used style first, stopping when the heap can no
  // longer afford the next one. Each style costs its own intervals, glyph array, bitmap
  // arena and mini kern matrix; prewarming all four of a 60-glyph definition took the X4
  // down to 5.9KB free / 2.1KB largest block, at which point the last style failed to
  // allocate anyway ("Failed to allocate mini bitmap") and everything downstream — the
  // wrap, pagePool_, the anti-aliasing pass — was running on fumes. A style left out here
  // still renders correctly; its glyphs just load on demand through the overflow ring.
  constexpr size_t kMinFreeForStyle = 16 * 1024;
  constexpr size_t kMinBlockForStyle = 8 * 1024;
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
      if (ESP.getFreeHeap() < kMinFreeForStyle ||
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kMinBlockForStyle) {
        LOG_DBG("DDA", "prewarm: heap floor reached, styles 0x%02X left on demand",
                static_cast<uint8_t>(collector->styleMask & ~warmedMask));
        break;
      }
      fcm->prewarmCache(defFontId_, collector->utf8.c_str(), bit);
      warmedMask |= bit;
    }
  }

  LOG_DBG("DDA", "prewarm: body=%u ipa=%u mask=0x%02X warmed=0x%02X scan=%lums ipa=%lums body=%lums free=%u largest=%u",
          collector->uniqueCount, collector->ipaCount, collector->styleMask, warmedMask, tScan - t0, tIpa - tScan,
          millis() - tIpa, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

// Re-parse the definition and lay out ONLY `page` into layoutLines. The wrap
// produces every line, but collectLineSink keeps only this page's lines (the
// rest are produced then dropped, so peak RAM is one page, not the whole
// definition) and counts all lines to recompute totalPages. Called on entry and
// on every page turn (Stage 2a: re-parse every turn, both directions).
void DictionaryDefinitionActivity::loadPage(int page) {
  layoutLines.clear();
  layoutLines.reserve(static_cast<size_t>(linesPerPage) + 1);
  pagePool_.clear();
  collectTargetPage_ = page;
  collectLineCount_ = 0;

  const unsigned long t0 = millis();
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->resetStats();  // attribute SD glyph I/O below to layout, not to the render

  // Choose rendering path based on dictionary content type (resolved in wrapText)
  if (defIsHtml_) {
    wrapHtml();
  } else {
    wrapPlain();
  }

  totalPages = DictLayout::paginate(collectLineCount_, linesPerPage);

  LOG_DBG("DDA", "loadPage %d: wrap=%lums lines=%d pages=%d", page, millis() - t0, collectLineCount_, totalPages);
  if (fcm) fcm->logStats("dict-wrap");
}

void DictionaryDefinitionActivity::collectLineSink(void* ctx, DictLayout::LayoutLine&& line) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int idx = self->collectLineCount_++;
  const int start = self->collectTargetPage_ * self->linesPerPage;
  if (idx < start || idx >= start + self->linesPerPage) return;  // not on this page — discard

  // Pool the kept line's text: each (already same-style-merged) segment becomes
  // one null-terminated pool entry referenced by {offset, len}.
  PooledLine pooled;
  pooled.indentLevel = line.indentLevel;
  pooled.isListItem = line.isListItem;
  pooled.segments.reserve(line.segments.size());
  for (const auto& seg : line.segments) {
    PooledSegment ps;
    ps.offset = TextPool::append(self->pagePool_, seg.text.c_str(), seg.text.size());
    ps.len = static_cast<uint16_t>(seg.text.size());
    ps.style = seg.style;
    ps.isIpa = seg.isIpa;
    pooled.segments.push_back(ps);
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
    return sum + renderer.getTextWidth(run.isIpa ? IPA_FONT_ID : defFontId_, run.text.c_str(), style);
  });
}

// ---------------------------------------------------------------------------
// HTML path: run DictHtmlRenderer, lay out spans into LayoutLines
// ---------------------------------------------------------------------------

int DictionaryDefinitionActivity::measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style,
                                                      bool isIpa) {
  auto* self = static_cast<DictionaryDefinitionActivity*>(ctx);
  const int fontId = isIpa ? IPA_FONT_ID : self->defFontId_;
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
    for (const auto& run : ipaRuns) {
      line.segments.push_back({run.text, EpdFontFamily::REGULAR, run.isIpa});
    }
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
  const int indentStep = renderer.getTextWidth(defFontId_, "   ");

  std::vector<WordSelectNavigator::WordInfo> words;
  words.reserve(64);
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
      const int segFontId = seg.isIpa ? IPA_FONT_ID : defFontId_;
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
        wrapText();  // resets currentPage to 0 and loads page 0
        if (wasBackNav) {
          // Re-derive the now-current word's history position and restore its page.
          chain_.setCurrentHistIndex(pendingBack_.histIndex);
          currentPage = (pendingBack_.page < totalPages) ? pendingBack_.page : (totalPages - 1);
          if (currentPage < 0) currentPage = 0;
          if (currentPage > 0) loadPage(currentPage);
        }
        isWordSelectMode = false;
        requestUpdate();
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
  GUI.drawHeader(renderer,
                 Rect{contentX, hintGutterHeight + metrics.topPadding, renderer.getScreenWidth() - hintGutterWidth,
                      metrics.headerHeight},
                 headword.c_str());

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
        const int segFontId = seg.isIpa ? IPA_FONT_ID : defFontId_;
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
}
