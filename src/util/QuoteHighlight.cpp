#include "QuoteHighlight.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "PageTokenScan.h"

namespace QuoteHighlight {
namespace {

constexpr int kUnderlineThickness = 2;

// Horizontal bleed on the highlight band, so it does not cut flush against the first and last
// glyph. The band's height is exactly the line height, which already contains the descent, so
// bands on consecutive rows cannot run into each other.
constexpr int kBandPadX = 1;

// Quotes drawn on a single page. Past this a page is no longer a reading page; the extras are
// skipped rather than grown into, so the whole walk stays on the stack — this runs on the
// render task, whose depth is already budgeted for EPUB section indexing.
constexpr size_t kMaxQuotesPerPage = 6;

// One quote being resolved, plus the marking run currently open for it. Runs are per visual
// line: a quote crossing three lines draws three rectangles, and each one spans from its first
// word to its last, so the punctuation between highlighted words — which carries no word index
// of its own — is covered too.
struct Range {
  uint16_t start = 0;
  uint16_t end = 0;
  const char* snippet = nullptr;
  bool checked = false;
  bool rejected = false;
  bool runOpen = false;
  int16_t runY = 0;
  int16_t runX0 = 0;
  int16_t runX1 = 0;
};

// Word indices only describe the pagination that produced them. Change the font, the margins or
// the orientation and the chapter re-flows: the quote's progress still lands on some page, but
// startWord now names an unrelated word. Checking the first word against the stored snippet
// turns that into "no mark" instead of a mark on the wrong sentence.
bool firstWordMatches(const char* snippet, const char* text, size_t len) {
  if (!snippet || !*snippet || len == 0) return true;  // nothing to check against
  // Trim a trailing '-' or soft hyphen in place rather than copying — a hyphenated first half
  // is stored merged ("externity"), so its own hyphen never appears in the snippet. Mirrors
  // utf8RemoveTrailingHyphen, which needs a std::string this path deliberately avoids.
  if (text[len - 1] == '-') {
    len--;
  } else if (len >= 2 && static_cast<uint8_t>(text[len - 2]) == 0xC2 && static_cast<uint8_t>(text[len - 1]) == 0xAD) {
    len -= 2;
  }
  if (len == 0) return true;
  if (std::strncmp(snippet, text, len) == 0) return true;

  // The other half of that pair: the snippet opens with the merged word while the token holds
  // only its tail ("nity"), so a match against the end of the snippet's first word counts too.
  const char* space = std::strchr(snippet, ' ');
  const size_t firstLen = space ? static_cast<size_t>(space - snippet) : std::strlen(snippet);
  return firstLen >= len && std::strncmp(snippet + firstLen - len, text, len) == 0;
}

}  // namespace

void drawForPage(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                 uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return;
  // markStyle, not style: the per-word EpdFontFamily::Style below would shadow it.
  const uint8_t markStyle = SETTINGS.quoteHighlightStyle;
  if (markStyle == CrossPointSettings::QUOTE_STYLE_OFF) return;

  // Page match mirrors BookmarkStore::hasQuoteForPage: the quote's progress falls inside this
  // page's slice of the chapter.
  const float pageSlice = 1.0f / static_cast<float>(pageCount);
  Range ranges[kMaxQuotesPerPage];
  size_t rangeCount = 0;
  for (const auto& b : BOOKMARKS.getBookmarks()) {
    if (!b.quote || b.spineIndex != spineIndex) continue;
    if (b.progress < pageProgress || b.progress >= pageProgress + pageSlice) continue;
    if (rangeCount >= kMaxQuotesPerPage) break;
    Range& r = ranges[rangeCount++];
    r.start = std::min(b.startWord, b.endWord);
    r.end = std::max(b.startWord, b.endWord);
    r.snippet = b.snippet;
  }
  if (rangeCount == 0) return;  // the common case: nothing walked, nothing measured

  const int lineHeight = renderer.getLineHeight(fontId);
  const int ascender = renderer.getFontAscenderSize(fontId);

  const auto flush = [&](Range& r) {
    if (!r.runOpen) return;
    const int width = r.runX1 - r.runX0;
    if (width > 0) {
      if (markStyle == CrossPointSettings::QUOTE_STYLE_UNDERLINE) {
        renderer.fillRect(r.runX0, r.runY + lineHeight - kUnderlineThickness, width, kUnderlineThickness, true);
      } else {
        // washRectDither, not fillRectDither: the text is already drawn here, and the plain
        // dither fill writes both inks and would wipe the glyphs out from under the band.
        renderer.washRectDither(r.runX0 - kBandPadX, r.runY, width + 2 * kBandPadX, lineHeight, Color::LightGray);
      }
    }
    r.runOpen = false;
  };

  uint16_t index = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;

    // Ruby-annotated lines shift their base text down by half an ascender, exactly as
    // extractWords moves the selection boxes in lockstep with them.
    const int16_t rowY = static_cast<int16_t>(line->yPos + marginTop + block->getRubyShift(ascender));
    const uint16_t blockWordCount = block->wordCount();

    for (uint16_t w = 0; w < blockWordCount; w++) {
      const char* text = block->wordText(w);
      const size_t len = block->wordTextLen(w);
      bool isCjk = false;
      if (!PageTokens::isSelectable(text, len, isCjk)) continue;  // no index, no mark

      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t partCount = PageTokens::collectParts(text, len, parts, PageTokens::kMaxTokenParts);
      const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == len;

      for (size_t pi = 0; pi < partCount; pi++, index++) {
        // Close any run whose quote ended before this token. Walking in index order means a
        // quote can never come back once it is behind us.
        for (size_t i = 0; i < rangeCount; i++) {
          if (ranges[i].runOpen && index > ranges[i].end) flush(ranges[i]);
        }

        const size_t partStart = unsplit ? 0 : parts[pi].start;
        const size_t partLen = unsplit ? len : parts[pi].end - parts[pi].start;

        // Everything outside a quote costs one predicate and an increment; geometry is measured
        // only for the handful of tokens actually being marked.
        bool measured = false;
        int16_t x = 0;
        int16_t width = 0;

        for (size_t i = 0; i < rangeCount; i++) {
          Range& r = ranges[i];
          if (r.rejected || index < r.start || index > r.end) continue;
          if (!r.checked) {
            r.checked = true;
            r.rejected = !firstWordMatches(r.snippet, text + partStart, partLen);
            if (r.rejected) {
              LOG_DBG("QHL", "Quote %u-%u: page re-flowed, anchor no longer matches - not drawn", (unsigned)r.start,
                      (unsigned)r.end);
              continue;
            }
          }
          if (!measured) {
            measured = true;
            const EpdFontFamily::Style style = block->wordStyle(w);
            x = static_cast<int16_t>(line->xPos + block->wordXpos(w) + marginLeft);
            if (partStart > 0) x += PageTokens::measureAdvance(renderer, fontId, text, partStart, style);
            // The whole-token form copies nothing; only a dash-split part needs the sub-range one.
            width = unsplit ? PageTokens::measureAdvance(renderer, fontId, text, style)
                            : PageTokens::measureAdvance(renderer, fontId, text + partStart, partLen, style);
          }
          if (r.runOpen && r.runY != rowY) flush(r);  // the quote wrapped onto the next line
          if (!r.runOpen) {
            r.runOpen = true;
            r.runY = rowY;
            r.runX0 = x;
            r.runX1 = static_cast<int16_t>(x + width);
          } else {
            r.runX1 = std::max(r.runX1, static_cast<int16_t>(x + width));
          }
        }
      }
    }
  }

  for (size_t i = 0; i < rangeCount; i++) flush(ranges[i]);
}

}  // namespace QuoteHighlight
