#include "PageMarks.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "LookupMarks.h"
#include "PageTokenScan.h"
#include "QuoteSpan.h"
#include "SnippetMatch.h"

namespace PageMarks {
namespace {

constexpr int kUnderlineThickness = 2;

// Byte length of one edge punctuation mark at the start (atEnd = false) or end of `t`, 0 when
// none: ASCII that is neither letter nor digit, and the UTF-8 quotes, dashes, ellipsis and
// CJK marks a page prints against a word. The lookup underline steps over these.
size_t edgePunct(const char* t, const size_t len, const bool atEnd) {
  if (len == 0) return 0;
  const auto u = [&](const size_t i) { return static_cast<uint8_t>(t[i]); };
  const uint8_t a = u(atEnd ? len - 1 : 0);
  if (a < 0x80) return std::isalnum(a) ? 0 : 1;
  if (len >= 2) {
    const size_t i = atEnd ? len - 2 : 0;
    if (u(i) == 0xC2 && (u(i + 1) == 0xAB || u(i + 1) == 0xBB || u(i + 1) == 0xA1 || u(i + 1) == 0xBF)) return 2;
  }
  if (len >= 3) {
    const size_t i = atEnd ? len - 3 : 0;
    const uint8_t b = u(i + 1), c = u(i + 2);
    if (u(i) == 0xE2 && b == 0x80 && ((c >= 0x93 && c <= 0x9E) || c == 0xA6 || c == 0xB9 || c == 0xBA)) return 3;
    if (u(i) == 0xE3 && b == 0x80 && ((c >= 0x81 && c <= 0x82) || (c >= 0x88 && c <= 0x91))) return 3;
    if (u(i) == 0xEF && b == 0xBC &&
        (c == 0x81 || c == 0x88 || c == 0x89 || c == 0x8C || c == 0x9A || c == 0x9B || c == 0x9F))
      return 3;
  }
  return 0;
}

// Looked-up words get a dotted rule, so they read apart from a link's solid underline.
constexpr int kLookupDotPitch = 4;  // one 2x2 dot every this many pixels

// Horizontal bleed on the highlight band, so it does not cut flush against the first and last
// glyph. The band's height is exactly the line height, which already contains the descent, so
// bands on consecutive rows cannot run into each other.
constexpr int kBandPadX = 1;

// Quotes drawn on a single page. Past this a page is no longer a reading page; the extras are
// skipped rather than grown into, so the whole walk stays on the stack — this runs on the
// render task, whose depth is already budgeted for EPUB section indexing.
constexpr size_t kMaxQuotesPerPage = 6;

// How far, in pages, a quote made on this device is looked for by its text when its saved
// page no longer holds it: a re-layout, or a page count that was still growing when it was
// saved, moves the text a few pages either way. Only snippets this long take part, since a
// short one ("the") would match somewhere on any page.
constexpr int kRelocatePages = 3;
constexpr size_t kRelocateMinSnippet = 8;

// Looked-up words marked on a single page. A page holds a handful of lookups at most; the
// extras are skipped rather than grown into, for the same stack-only reason as the quote cap.
constexpr size_t kMaxLookupsPerPage = 8;

// The screen span a mark covers so far, carried alongside the accumulator that decides whether
// there is a mark at all. The matching itself is LookupMarks::step -- this side is only the
// geometry that arithmetic has no business knowing about, which is what lets the same predicate
// serve both drawForPage and lookupMarkAtPoint.
struct LookupRun {
  LookupMarks::RunState state;
  // The segment the run is on now.
  int16_t x0 = 0;
  int16_t x1 = 0;
  int16_t y = 0;
  const PageLine* line = nullptr;
  uint16_t wStart = 0;  // first and last block word of the segment, on `line`
  uint16_t wLast = 0;
  // The first segment of a run that crossed its one line break (state.wrapped), else unused.
  int16_t headX0 = 0;
  int16_t headX1 = 0;
  int16_t headY = 0;
  const PageLine* headLine = nullptr;
  uint16_t headWStart = 0;
  uint16_t headWLast = 0;
};

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
  // A mark a KOReader peer made: its word range describes that reader's layout, so the
  // range above is resolved from the quoted text instead (resolveForeignRanges).
  bool foreign = false;
  // A local quote: tried at its saved word first, then located by its text when that word no
  // longer matches. `nearby` is one saved for another page, located by its text alone.
  bool local = false;
  bool nearby = false;
  bool indexOk = false;
  // Which part of the quote this page holds. A peer selects against a larger page, so one
  // of its highlights routinely covers two or three here: the first page runs from the
  // matched word to the page end, the last from the page start to where the quote stops,
  // and any page between is highlighted whole.
  bool startsHere = true;
  bool endsHere = true;
  // Where in this page the quote stops, in 255ths of the page, for the page that holds its
  // end but not its start. There are no per-word character offsets to place it exactly --
  // the section cache records one offset per PAGE -- so this interpolates over the page's
  // tokens and can be a word or two out. Only ever shortens a run. A byte because six of
  // these sit on a render-path frame, and 1/255 of a page is already finer than the
  // interpolation it feeds.
  uint8_t endFracInPage = 255;
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

// Resolve the word range of every foreign quote on this page, and settle where each local
// one is (its saved word, else its text), in one walk. Measuring
// nothing: this only needs the token sequence, and it has to complete before any band is
// drawn, or a false partial match would paint one.
void resolveForeignRanges(const Page& page, Range* ranges, size_t rangeCount) {
  SnippetMatch::Matcher matchers[kMaxQuotesPerPage];
  size_t pending = 0;
  size_t continuing = 0;
  for (size_t i = 0; i < rangeCount; i++) {
    if (!ranges[i].foreign && !ranges[i].local) continue;
    if (ranges[i].local) {
      if (!ranges[i].snippet || !*ranges[i].snippet) {
        ranges[i].indexOk = true;  // nothing to check its word against, as firstWordMatches
        continue;
      }
      matchers[i].snippet = ranges[i].snippet;
      matchers[i].begin(BOOKMARK_SNIPPET_MAX);
      pending++;
      continue;
    }
    if (!ranges[i].startsHere) {
      // The quote began on an earlier page, so it covers this one from its first token.
      // There is nothing to match: the snippet holds the quote's OPENING words, which are
      // not on this page, and for a long highlight it never held its tail at all.
      continuing++;
      continue;
    }
    if (!ranges[i].snippet || !*ranges[i].snippet) {
      ranges[i].rejected = true;
      continue;
    }
    matchers[i].snippet = ranges[i].snippet;
    matchers[i].begin(BOOKMARK_SNIPPET_MAX);
    pending++;
  }
  if (pending == 0 && continuing == 0) return;

  uint16_t matchEnd[kMaxQuotesPerPage] = {};
  uint16_t index = 0;
  uint16_t lastIndex = 0;
  bool seenAny = false;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const uint16_t blockWordCount = block->wordCount();
    for (uint16_t w = 0; w < blockWordCount; w++) {
      const char* text = block->wordText(w);
      const size_t len = block->wordTextLen(w);
      bool isCjk = false;
      if (!PageTokens::isSelectable(text, len, isCjk)) continue;

      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t partCount = PageTokens::collectParts(text, len, parts, PageTokens::kMaxTokenParts);
      const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == len;
      for (size_t pi = 0; pi < partCount; pi++, index++) {
        const size_t partStart = unsplit ? 0 : parts[pi].start;
        const size_t partLen = unsplit ? len : parts[pi].end - parts[pi].start;
        lastIndex = index;
        seenAny = true;
        for (size_t i = 0; i < rangeCount; i++) {
          Range& lr = ranges[i];
          if (lr.local && !lr.nearby && index == lr.start) {
            lr.indexOk = firstWordMatches(lr.snippet, text + partStart, partLen);
          }
          if (lr.local) {
            if (!lr.indexOk && !matchers[i].done && matchers[i].snippet &&
                matchers[i].offer(text + partStart, partLen, index)) {
              matchEnd[i] = index;
            }
            continue;
          }
          if (!ranges[i].foreign || !ranges[i].startsHere || matchers[i].done || !matchers[i].snippet) continue;
          if (matchers[i].offer(text + partStart, partLen, index)) {
            ranges[i].start = matchers[i].start;
            ranges[i].end = index;
            // Already located by its text; the first-word check downstream would only
            // repeat what this just proved.
            ranges[i].checked = true;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < rangeCount; i++) {
    Range& r = ranges[i];
    if (!r.local) continue;
    if (r.indexOk) {
      r.checked = true;
    } else if (matchers[i].done) {
      // Its saved word moved, its text did not: draw it where the text is now.
      r.start = matchers[i].start;
      r.end = matchEnd[i];
      r.checked = true;
    } else {
      r.rejected = true;
      if (!r.nearby) {
        static const char* lastLocal = nullptr;
        if (r.snippet && r.snippet != lastLocal) {
          lastLocal = r.snippet;
          SdDebugLog::log("PGM", "quote unmatched on its page: \"%.63s\"", r.snippet);
        }
      }
    }
  }

  for (size_t i = 0; i < rangeCount; i++) {
    Range& r = ranges[i];
    if (!r.foreign || r.rejected) continue;

    // Where the quote stops on this page: its own end when it ends here, the last token
    // otherwise. Interpolating over tokens is the best available -- the section cache
    // records a visible-character offset per page, not per word -- and only ever pulls the
    // end in, so an error shortens the band rather than marking text outside the quote.
    const uint16_t pageLast = seenAny ? lastIndex : 0;
    uint16_t stopAt = pageLast;
    if (r.endsHere && seenAny) stopAt = QuoteSpan::stopIndex(r.endFracInPage, pageLast);

    if (!r.startsHere) {
      // Continuation: from the top of the page. Located by the quote's page span rather
      // than by its text, so there is no first-word check to make.
      if (!seenAny) {
        r.rejected = true;
        continue;
      }
      r.start = 0;
      r.end = stopAt;
      r.checked = true;
      continue;
    }

    if (matchers[i].done) {
      // Matched in full. It still runs to the page edge when the quote carries on past it.
      if (!r.endsHere) r.end = pageLast;
      continue;
    }

    // The snippet ran out of page mid-run. That is exactly what a quote crossing the break
    // looks like from here, so accept it -- but only when the quote is independently known
    // to continue. A quote that both starts and ends on this page must still match whole,
    // or a page sharing the highlight's opening words would draw a band it has no text for.
    if (!r.endsHere && matchers[i].open) {
      r.start = matchers[i].start;
      r.end = pageLast;
      r.checked = true;
      continue;
    }

    // No band: a partial one would claim a length the mark does not have.
    r.rejected = true;
    // A range only reaches here when the mark's progress already put it on this page, so a
    // rejection means the text did not line up -- the one failure this walk can have that
    // leaves nothing on screen and nothing in any other log. Deduped against the last one,
    // since a page redraws far more often than its marks change.
    static const char* lastReported = nullptr;
    if (r.snippet && r.snippet != lastReported) {
      lastReported = r.snippet;
      SdDebugLog::log("PGM", "foreign quote unmatched: \"%.63s\"", r.snippet);
    }
  }
}

}  // namespace

bool pageHasWord(const Page& page, const LookupMarks::Mark& mark) {
  LookupMarks::RunState state;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const uint16_t blockWordCount = block->wordCount();
    for (uint16_t w = 0; w < blockWordCount; w++) {
      const char* text = block->wordText(w);
      const size_t len = block->wordTextLen(w);
      bool isCjk = false;
      if (!PageTokens::isSelectable(text, len, isCjk)) continue;
      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t partCount = PageTokens::collectParts(text, len, parts, PageTokens::kMaxTokenParts);
      const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == len;
      for (size_t pi = 0; pi < partCount; pi++) {
        const size_t partStart = unsplit ? 0 : parts[pi].start;
        const size_t partLen = unsplit ? len : parts[pi].end - parts[pi].start;
        uint16_t tokenLen = 0;
        const uint32_t tokenHash =
            LookupMarks::hashAppend(LookupMarks::FNV_OFFSET, text + partStart, partLen, &tokenLen);
        const auto step = LookupMarks::step(mark, state, isCjk, tokenHash, tokenLen, text + partStart, partLen,
                                            static_cast<int16_t>(line->yPos));
        if (step == LookupMarks::Step::MatchedToken || step == LookupMarks::Step::MatchedRun) return true;
      }
    }
  }
  return false;
}

SnippetAt findSnippet(const Page& page, const char* snippet) {
  if (!snippet || std::strlen(snippet) < kRelocateMinSnippet) return SnippetAt::None;
  SnippetMatch::Matcher matcher;
  matcher.snippet = snippet;
  matcher.begin(BOOKMARK_SNIPPET_MAX);
  uint16_t index = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const uint16_t blockWordCount = block->wordCount();
    for (uint16_t w = 0; w < blockWordCount; w++) {
      const char* text = block->wordText(w);
      const size_t len = block->wordTextLen(w);
      bool isCjk = false;
      if (!PageTokens::isSelectable(text, len, isCjk)) continue;
      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t partCount = PageTokens::collectParts(text, len, parts, PageTokens::kMaxTokenParts);
      const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == len;
      for (size_t pi = 0; pi < partCount; pi++, index++) {
        const size_t partStart = unsplit ? 0 : parts[pi].start;
        const size_t partLen = unsplit ? len : parts[pi].end - parts[pi].start;
        if (matcher.offer(text + partStart, partLen, index)) return SnippetAt::Whole;
      }
    }
  }
  return matcher.open ? SnippetAt::RunsOff : SnippetAt::None;
}

void drawForPage(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                 uint16_t spineIndex, float pageProgress, int pageCount, uint32_t chapterHash, int pageNumber,
                 const int markPageCount, const int bandTop, const int bandBottom) {
  if (pageCount <= 0) return;
  // markStyle, not style: the per-word EpdFontFamily::Style below would shadow it.
  const uint8_t markStyle = SETTINGS.quoteHighlightStyle;

  // Page match mirrors BookmarkStore::hasQuoteForPage: the quote's progress falls inside this
  // page's slice of the chapter.
  Range ranges[kMaxQuotesPerPage];
  size_t rangeCount = 0;
  if (markStyle != CrossPointSettings::QUOTE_STYLE_OFF) {
    const float pageSlice = 1.0f / static_cast<float>(pageCount);
    for (const auto& b : BOOKMARKS.getBookmarks()) {
      if (!b.quote || b.spineIndex != spineIndex) continue;
      // endProgress equals progress for a quote made here -- a selection on this device
      // cannot cross a page break -- so this reduces to the old start-page test for them.
      const QuoteSpan::Role role = QuoteSpan::pageRole(b.progress, b.endProgress, pageProgress, pageSlice);
      const bool foreign = BookmarkStore::isForeignMark(b);
      // A local quote is single-page (endProgress == progress); one saved for a nearby page
      // is offered to this page too, to be drawn only if its text is here.
      const float pageOffset = (b.progress - pageProgress) / pageSlice;
      const bool nearby = role == QuoteSpan::Role::NotHere && !foreign && pageOffset > -(kRelocatePages + 1) &&
                          pageOffset < kRelocatePages + 1 && std::strlen(b.snippet) >= kRelocateMinSnippet;
      if (role == QuoteSpan::Role::NotHere && !nearby) continue;
      if (rangeCount >= kMaxQuotesPerPage) break;
      Range& r = ranges[rangeCount++];
      r.start = std::min(b.startWord, b.endWord);
      r.end = std::max(b.startWord, b.endWord);
      r.snippet = b.snippet;
      r.foreign = foreign;
      r.local = !foreign;
      r.nearby = nearby;
      r.startsHere = nearby || role == QuoteSpan::Role::Whole || role == QuoteSpan::Role::Starts;
      r.endsHere = nearby || role == QuoteSpan::Role::Whole || role == QuoteSpan::Role::Ends;
      r.endFracInPage = QuoteSpan::endFraction(b.endProgress, pageProgress, pageSlice);
    }
    resolveForeignRanges(page, ranges, rangeCount);
  }

  // Looked-up words anchored here. Resolved against the resident table, not the page: the
  // deck records where a lookup happened as the chapter and in-chapter page, so this costs
  // one scan of a small array and no I/O.
  const LookupMarks::Mark* lookups[kMaxLookupsPerPage] = {};
  LookupRun runs[kMaxLookupsPerPage] = {};
  size_t lookupCount = 0;
  if (SETTINGS.lookupUnderline) {
    lookupCount = static_cast<size_t>(
        LookupMarks::getInstance().collectForPage(chapterHash, pageNumber, markPageCount, lookups, kMaxLookupsPerPage));
  }

  if (rangeCount == 0 && lookupCount == 0) return;  // the common case: nothing walked, nothing measured

  const int lineHeight = renderer.getLineHeight(fontId);
  const int ascender = renderer.getFontAscenderSize(fontId);

  // One rule under a span of a line: solid for a quote in the underline style, dotted for a
  // looked-up word.
  const auto inBand = [&](const int rowY) { return rowY < bandBottom && rowY + lineHeight > bandTop; };

  const auto underlineSpan = [&](int16_t x0, int width, int16_t rowY) {
    if (width > 0 && inBand(rowY))
      renderer.fillRect(x0, rowY + lineHeight - kUnderlineThickness, width, kUnderlineThickness, true);
  };
  const auto lookupSpan = [&](int16_t x0, int width, int16_t rowY) {
    if (width <= 0 || !inBand(rowY)) return;
    const int y = rowY + lineHeight - kUnderlineThickness;
    for (int dx = 0; dx < width; dx += kLookupDotPitch) {
      renderer.fillRect(x0 + dx, y, std::min(kUnderlineThickness, width - dx), kUnderlineThickness, true);
    }
  };

  const auto flush = [&](Range& r) {
    if (!r.runOpen) return;
    const int width = r.runX1 - r.runX0;
    if (width > 0) {
      if (markStyle == CrossPointSettings::QUOTE_STYLE_UNDERLINE) {
        underlineSpan(r.runX0, width, r.runY);
      } else {
        // washRectDither, not fillRectDither: the text is already drawn here, and the plain
        // dither fill writes both inks and would wipe the glyphs out from under the band.
        if (inBand(r.runY)) {
          renderer.washRectDither(r.runX0 - kBandPadX, r.runY, width + 2 * kBandPadX, lineHeight, Color::LightGray);
        }
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
    const int8_t tracking = block->getBlockStyle().characterSpacing;

    // Ruby-annotated lines shift their base text down by half an ascender, exactly as
    // extractWords moves the selection boxes in lockstep with them.
    const int16_t rowY = static_cast<int16_t>(line->yPos + marginTop + block->getRubyShift(ascender));
    const uint16_t blockWordCount = block->wordCount();

    // A looked-up CJK word is also thickened by redrawing its glyphs offset right, down and
    // diagonally, which widens every stroke by a pixel in both axes. An overdraw rather than the
    // BOLD face: a font without one silently falls back to regular (SdCardFont::resolveStyle).
    // SUP/SUB tokens are left alone -- their baseline is shifted inside TextBlock::render and not
    // reproduced here.
    const auto emboldenCjk = [&](const PageLine* segLine, const uint16_t wFirst, const uint16_t wLast) {
      const auto& segBlock = segLine->getBlock();
      const int16_t segY = static_cast<int16_t>(segLine->yPos + marginTop + segBlock->getRubyShift(ascender));
      if (!inBand(segY)) return;
      for (uint16_t wi = wFirst; wi <= wLast; wi++) {
        const char* t = segBlock->wordText(wi);
        bool cjk = false;
        if (!PageTokens::isSelectable(t, segBlock->wordTextLen(wi), cjk) || !cjk) continue;
        const EpdFontFamily::Style style = segBlock->wordStyle(wi);
        if ((style & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) continue;
        const int x0 = segLine->xPos + segBlock->wordXpos(wi) + marginLeft;
        renderer.drawText(fontId, x0 + 1, segY, t, true, style);
        renderer.drawText(fontId, x0, segY + 1, t, true, style);
        renderer.drawText(fontId, x0 + 1, segY + 1, t, true, style);
      }
    };

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

        // Everything outside a mark costs one predicate and an increment; geometry is measured
        // only for the handful of tokens actually being marked.
        bool measured = false;
        int16_t x = 0;
        int16_t width = 0;
        const auto measure = [&]() {
          if (measured) return;
          measured = true;
          const EpdFontFamily::Style style = block->wordStyle(w);
          x = static_cast<int16_t>(line->xPos + block->wordXpos(w) + marginLeft);
          if (partStart > 0) x += PageTokens::measureAdvance(renderer, fontId, text, partStart, style, tracking);
          // The whole-token form copies nothing; only a dash-split part needs the sub-range one.
          width = unsplit ? PageTokens::measureAdvance(renderer, fontId, text, style, tracking)
                          : PageTokens::measureAdvance(renderer, fontId, text + partStart, partLen, style, tracking);
        };

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
          measure();
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

        if (lookupCount == 0) continue;

        // One hash of the token's own bytes serves every mark on the page: a byte loop over a
        // token that is already in cache, no allocation, no measurement.
        uint16_t tokenLen = 0;
        const uint32_t tokenHash =
            LookupMarks::hashAppend(LookupMarks::FNV_OFFSET, text + partStart, partLen, &tokenLen);

        // The token's span minus its edge punctuation: the lookup rule marks the word only.
        bool trimmed = false;
        int16_t lx = 0;
        int16_t lw = 0;
        const auto trimToWord = [&]() {
          if (trimmed) return;
          trimmed = true;
          measure();
          lx = x;
          lw = width;
          const char* t = text + partStart;
          size_t lead = 0, trail = 0;
          for (size_t n; (n = edgePunct(t + lead, partLen - lead - trail, false)) != 0;) lead += n;
          for (size_t n; (n = edgePunct(t + lead, partLen - lead - trail, true)) != 0;) trail += n;
          if ((lead == 0 && trail == 0) || lead + trail >= partLen) return;
          const EpdFontFamily::Style style = block->wordStyle(w);
          if (lead > 0)
            lx = static_cast<int16_t>(x + PageTokens::measureAdvance(renderer, fontId, t, lead, style, tracking));
          lw = PageTokens::measureAdvance(renderer, fontId, t + lead, partLen - lead - trail, style, tracking);
        };

        for (size_t i = 0; i < lookupCount; i++) {
          const LookupMarks::Mark* m = lookups[i];
          LookupRun& r = runs[i];

          using Step = LookupMarks::Step;
          const Step outcome =
              LookupMarks::step(*m, r.state, isCjk, tokenHash, tokenLen, text + partStart, partLen, rowY);
          if (outcome == Step::None) continue;
          trimToWord();
          if (r.state.wrappedHere) {
            // The run just crossed its line break: what it held so far is the first segment.
            r.headX0 = r.x0;
            r.headX1 = r.x1;
            r.headY = r.y;
            r.headLine = r.line;
            r.headWStart = r.wStart;
            r.headWLast = r.wLast;
            r.x0 = lx;
            r.x1 = lx;
            r.y = rowY;
            r.line = line;
            r.wStart = w;
          }
          switch (outcome) {
            case Step::Opened:
              r.x0 = lx;
              r.x1 = static_cast<int16_t>(lx + lw);
              r.y = rowY;
              r.line = line;
              r.wStart = w;
              r.wLast = w;
              break;
            case Step::Extended:
              r.x1 = std::max(r.x1, static_cast<int16_t>(lx + lw));
              r.wLast = w;
              break;
            case Step::MatchedRun:
              if (r.state.wrapped) {
                lookupSpan(r.headX0, r.headX1 - r.headX0, r.headY);
                emboldenCjk(r.headLine, r.headWStart, r.headWLast);
              }
              lookupSpan(r.x0, std::max<int16_t>(r.x1, static_cast<int16_t>(lx + lw)) - r.x0, r.state.y);
              emboldenCjk(line, r.wStart, w);
              break;
            case Step::MatchedToken:
              lookupSpan(lx, lw, rowY);
              if (isCjk) emboldenCjk(line, w, w);
              break;
            case Step::None:
              break;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < rangeCount; i++) flush(ranges[i]);
}

bool invertWordAtPoint(const GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                       const int marginTop, const int x, const int y, WordHit& out) {
  // Same slop as WordSelectNavigator::wordIndexAtPoint's default, so a tap that lands in the
  // padding of a box here lands in the same box there.
  constexpr int kSlop = 4;
  // Band padding, and therefore the pushed rect: WordSelectNavigator::boundsForWord.
  constexpr int kBandPad = 2;

  const int lineHeight = renderer.getLineHeight(fontId);
  const int ascender = renderer.getFontAscenderSize(fontId);
  // Fallback for blocks with no derivable per-line gap, as in extractWords.
  const int16_t naturalSpaceWidth = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, " ", EpdFontFamily::REGULAR));

  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const int8_t tracking = block->getBlockStyle().characterSpacing;

    const uint16_t blockWordCount = block->wordCount();
    // Ruby-annotated lines shift their base text down, and extractWords moves the tap boxes
    // with them, so the band has to follow too.
    const int16_t rowY = static_cast<int16_t>(line->yPos + marginTop + block->getRubyShift(ascender));
    if (y < rowY - kSlop || y >= rowY + lineHeight + kSlop) continue;  // wrong row: no measuring

    // Per-line inter-word gap, derived from the first word's xpos diff; a justified line
    // stretches it, so it cannot be a global space width. The half-space threshold separates a
    // real gap from a continuation token's kerning. Verbatim from extractWords, because an
    // undersized gap widens every box on the line and swallows the space after the word.
    int16_t lineGapWidth = naturalSpaceWidth;
    if (blockWordCount >= 2 && block->wordTextLen(0) > 0) {
      const int16_t firstWidth =
          PageTokens::measureAdvance(renderer, fontId, block->wordText(0), block->wordStyle(0), tracking);
      const int16_t derivedGap = static_cast<int16_t>(block->wordXpos(1) - block->wordXpos(0) - firstWidth);
      if (derivedGap > naturalSpaceWidth / 2) lineGapWidth = derivedGap;
    }

    for (uint16_t w = 0; w < blockWordCount; w++) {
      const char* text = block->wordText(w);
      const size_t len = block->wordTextLen(w);
      bool isCjk = false;
      if (!PageTokens::isSelectable(text, len, isCjk)) continue;  // no cursor stop, no tap target

      const EpdFontFamily::Style style = block->wordStyle(w);
      const int16_t screenX = static_cast<int16_t>(line->xPos + block->wordXpos(w) + marginLeft);

      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t partCount = PageTokens::collectParts(text, len, parts, PageTokens::kMaxTokenParts);
      const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == len;

      for (size_t pi = 0; pi < partCount; pi++) {
        const size_t partStart = unsplit ? 0 : parts[pi].start;
        const size_t partLen = unsplit ? len : parts[pi].end - parts[pi].start;
        int16_t boxX = screenX;
        int16_t boxWidth;
        if (!unsplit) {
          if (partStart > 0) boxX += PageTokens::measureAdvance(renderer, fontId, text, partStart, style, tracking);
          boxWidth = PageTokens::measureAdvance(renderer, fontId, text + partStart, partLen, style, tracking);
        } else if (isCjk) {
          // CJK carries no inter-word gap for the xpos diff to subtract, and a justified CJK
          // line hides justifyExtra in it; measuring is exact and the glyph is already cached.
          boxWidth = PageTokens::measureAdvance(renderer, fontId, text, style, tracking);
        } else if (w + 1 < blockWordCount) {
          // The layout's xpos diff with the trailing inter-word gap removed. Punctuation
          // tokens keep their xpos entry as a boundary marker, so the next token's is always
          // there to subtract from.
          const int16_t raw = static_cast<int16_t>(block->wordXpos(w + 1) - block->wordXpos(w));
          boxWidth = std::max(static_cast<int16_t>(1), static_cast<int16_t>(raw - lineGapWidth));
        } else {
          boxWidth = PageTokens::measureAdvance(renderer, fontId, text, style, tracking);  // no next xpos
        }

        if (x < boxX - kSlop || x >= boxX + boxWidth + kSlop) continue;

        // The band is redrawn from the token's own bytes, not from out.text, so an over-long
        // token still gets its glyphs back in white after the fill.
        char partBuf[64];
        const char* glyphs = text;
        if (!unsplit) {
          const int n = snprintf(partBuf, sizeof(partBuf), "%.*s", static_cast<int>(partLen), text + partStart);
          partBuf[utf8SafeTruncateBuffer(partBuf, std::min(n, static_cast<int>(sizeof(partBuf) - 1)))] = '\0';
          glyphs = partBuf;
        }

        out.x = boxX - kBandPad;
        out.y = rowY - kBandPad;
        out.width = boxWidth + 2 * kBandPad;
        out.height = lineHeight + 2 * kBandPad;
        const int copied = snprintf(out.text, sizeof(out.text), "%.*s", static_cast<int>(partLen), text + partStart);
        // snprintf truncates by bytes; cut back to the last whole codepoint so a CJK token can
        // never end in a partial sequence.
        out.text[utf8SafeTruncateBuffer(out.text, std::min(copied, static_cast<int>(sizeof(out.text) - 1)))] = '\0';

        renderer.fillRect(out.x, out.y, out.width, out.height, true);
        renderer.drawText(fontId, boxX, rowY, glyphs, false, style);
        return true;
      }
    }
  }
  return false;
}

const LookupMarks::Mark* lookupMarkAtPoint(const GfxRenderer& renderer, const Page& page, const int fontId,
                                           const int marginLeft, const int marginTop, const int x, const int y,
                                           const uint32_t chapterHash, const int pageNumber, const int pageCount) {
  // Same forgiveness the tap boxes carry, so a point on the edge of an underlined word still
  // finds it.
  constexpr int kSlop = 4;

  if (!SETTINGS.lookupUnderline) return nullptr;
  const LookupMarks::Mark* lookups[kMaxLookupsPerPage] = {};
  LookupRun runs[kMaxLookupsPerPage] = {};
  const size_t lookupCount = static_cast<size_t>(
      LookupMarks::getInstance().collectForPage(chapterHash, pageNumber, pageCount, lookups, kMaxLookupsPerPage));
  if (lookupCount == 0) return nullptr;

  const int lineHeight = renderer.getLineHeight(fontId);
  const int ascender = renderer.getFontAscenderSize(fontId);

  // The point falls on a span drawn at rowY, within kSlop of both edges.
  const auto covers = [&](const int16_t x0, const int16_t spanWidth, const int16_t rowY) {
    if (spanWidth <= 0) return false;
    if (y < rowY - kSlop || y >= rowY + lineHeight + kSlop) return false;
    return x >= x0 - kSlop && x < x0 + spanWidth + kSlop;
  };

  // The lookup half of drawForPage's walk, asking which mark covers a point instead of inking
  // every one of them. Separate rather than folded into invertWordAtPoint above: that stops at
  // the token under the finger, while a CJK run needs the tokens on either side of it. Both
  // drive LookupMarks::step, so this cannot name a word the underline did not mark.
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block) continue;
    const int8_t tracking = block->getBlockStyle().characterSpacing;

    const int16_t rowY = static_cast<int16_t>(line->yPos + marginTop + block->getRubyShift(ascender));
    const uint16_t blockWordCount = block->wordCount();

    for (uint16_t w = 0; w < blockWordCount; w++) {
      const char* text = block->wordText(w);
      const size_t len = block->wordTextLen(w);
      bool isCjk = false;
      if (!PageTokens::isSelectable(text, len, isCjk)) continue;

      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t partCount = PageTokens::collectParts(text, len, parts, PageTokens::kMaxTokenParts);
      const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == len;

      for (size_t pi = 0; pi < partCount; pi++) {
        const size_t partStart = unsplit ? 0 : parts[pi].start;
        const size_t partLen = unsplit ? len : parts[pi].end - parts[pi].start;

        bool measured = false;
        int16_t tokenX = 0;
        int16_t tokenWidth = 0;
        const auto measure = [&]() {
          if (measured) return;
          measured = true;
          const EpdFontFamily::Style style = block->wordStyle(w);
          tokenX = static_cast<int16_t>(line->xPos + block->wordXpos(w) + marginLeft);
          if (partStart > 0) tokenX += PageTokens::measureAdvance(renderer, fontId, text, partStart, style, tracking);
          tokenWidth = unsplit
                           ? PageTokens::measureAdvance(renderer, fontId, text, style, tracking)
                           : PageTokens::measureAdvance(renderer, fontId, text + partStart, partLen, style, tracking);
        };

        uint16_t tokenLen = 0;
        const uint32_t tokenHash =
            LookupMarks::hashAppend(LookupMarks::FNV_OFFSET, text + partStart, partLen, &tokenLen);

        for (size_t i = 0; i < lookupCount; i++) {
          const LookupMarks::Mark* m = lookups[i];
          LookupRun& r = runs[i];
          using Step = LookupMarks::Step;
          const Step outcome =
              LookupMarks::step(*m, r.state, isCjk, tokenHash, tokenLen, text + partStart, partLen, rowY);
          if (outcome == Step::None) continue;
          measure();
          if (r.state.wrappedHere) {
            r.headX0 = r.x0;
            r.headX1 = r.x1;
            r.headY = r.y;
            r.x0 = tokenX;
            r.x1 = tokenX;
            r.y = rowY;
          }
          switch (outcome) {
            case Step::Opened:
              r.x0 = tokenX;
              r.x1 = static_cast<int16_t>(tokenX + tokenWidth);
              r.y = rowY;
              break;
            case Step::Extended:
              r.x1 = std::max(r.x1, static_cast<int16_t>(tokenX + tokenWidth));
              break;
            case Step::MatchedRun: {
              const int16_t x1 = std::max(r.x1, static_cast<int16_t>(tokenX + tokenWidth));
              if (covers(r.x0, static_cast<int16_t>(x1 - r.x0), r.state.y)) return m;
              if (r.state.wrapped && covers(r.headX0, static_cast<int16_t>(r.headX1 - r.headX0), r.headY)) return m;
              break;
            }
            case Step::MatchedToken:
              if (covers(tokenX, tokenWidth, rowY)) return m;
              break;
            case Step::None:
              break;
          }
        }
      }
    }
  }
  return nullptr;
}

}  // namespace PageMarks
