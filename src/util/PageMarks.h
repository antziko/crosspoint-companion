#pragma once

#include <cstddef>
#include <cstdint>

#include "LookupMarks.h"

class GfxRenderer;
class Page;

// The marks the reader draws on top of a rendered page: saved quotes ("highlights") and the
// words looked up in the dictionary. Both are resolved by walking the page's tokens in the
// order PageTokens numbers them, so they share one walk and one set of measurements.
namespace PageMarks {

// True when `page` prints the word `mark` identifies (LookupMarks::markFor), by the same token
// matching the lookup underline uses. Allocates nothing.
bool pageHasWord(const Page& page, const LookupMarks::Mark& mark);

// Where `page` prints a mark's opening words (Bookmark::snippet): all of them, or a start at its
// foot that the next page carries on. Snippets shorter than the nearby-relocation minimum never
// match. Allocates nothing.
enum class SnippetAt : uint8_t { None, Whole, RunsOff };

// Diagnostics for a snippet that would not match: the furthest any run of it got (bytes of the
// snippet consumed), the page that run was on, and the snippet part it expected against the page
// token it got instead. Accumulates over calls, so one instance can follow a whole-chapter search;
// the caller sets `curPage` before each call.
struct SnippetMiss {
  int curPage = -1;
  int page = -1;
  uint8_t best = 0;
  char want[24] = {};
  char got[24] = {};
  char gotHex[40] = {};
};
// `startIndex`, when given, receives the page-local token index the match opened at.
SnippetAt findSnippet(const Page& page, const char* snippet, SnippetMiss* miss = nullptr,
                      uint16_t* startIndex = nullptr);

// A point bookmark's extent. It records how many words its page held and is re-found by its
// opening words, so after a re-flow its end is that many words further on. A "span word" is a
// token part not ending in a hyphen: a word broken across two lines then counts once, wherever
// the layout breaks it. Both allocate nothing.
uint16_t countSpanWords(const Page& page);

// Counts span words off `remaining` from token `fromIndex` on. True when the count runs out
// on this page, `endIndex` then being the token it ran out on; otherwise `remaining` is what
// the following page still has to cover.
bool advanceSpan(const Page& page, uint16_t fromIndex, uint16_t& remaining, uint16_t& endIndex);

// Draws a bookmark's marks in the left margin at `barX`, for tokens fromIndex..toIndex
// (UINT16_MAX: past the page end): a bold block `blockWidth` wide beside the line it starts on,
// and, when it has a recorded length (`hasLength`), the same beside the line it ends on. A page
// it only passes through gets nothing. Nothing is drawn inside the text.
void drawBookmarkSpan(const GfxRenderer& renderer, const Page& page, int fontId, int marginTop, int barX,
                      uint16_t fromIndex, uint16_t toIndex, bool startsHere, bool endsHere, bool hasLength,
                      int blockWidth);

// Marks every quote anchored on this page and underlines every word looked up on it, in the
// coordinate space the page was just rendered in. Quotes draw as a 25% dither band behind the
// words or a rule under them, per SETTINGS.quoteHighlightStyle; looked-up words always draw as
// a rule, per SETTINGS.lookupUnderline.
// Call after Page::render() on a black-and-white pass.
//
// A quote stores its selection as page-local word indices (BookmarkStore.h:60-64), which only
// exist as numbers — turning them back into pixels means walking the page's tokens in the same
// order DictionaryWordSelectActivity::extractWords numbered them, which is why both walks share
// PageTokenScan. A looked-up word is anchored instead by its chapter and in-chapter page (see
// LookupMarks), so `chapterHash`, the 1-based `pageNumber` and `markPageCount` (the chapter's
// best-known total, Section::estimatedTotalPages -- the same figure the card recorded) identify
// the page it belongs to.
// The walk allocates nothing and is skipped entirely when neither kind of mark lands here,
// which is nearly every page.
// `bandTop`/`bandBottom` limit drawing to rows overlapping that screen band (the rest of the walk
// still runs, so a word wrapped into the band is still recognised): for a repaint of one strip
// of a page whose other rows already carry their marks -- or carry a selection highlight that a
// mark drawn over it would spoil.
//
// `placements`, when given, are this device's quotes already located across the chapter by
// their opening words and length (the reader's span resolver): a quote listed there is drawn
// from its token range on this page, including a page it only continues onto, and on no other
// page. A quote the list omits falls back to being found on this page alone, which cannot
// draw one a re-flow split across a page break.
struct QuotePlacement {
  const char* snippet = nullptr;  // identity: the BookmarkStore entry's own snippet buffer
  bool onThisPage = false;
  uint16_t start = 0;
  uint16_t end = 0;  // inclusive; UINT16_MAX = to the page end
};
void drawForPage(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                 uint16_t spineIndex, float pageProgress, int pageCount, uint32_t chapterHash, int pageNumber,
                 int markPageCount, int bandTop = INT16_MIN, int bandBottom = INT16_MAX,
                 const QuotePlacement* placements = nullptr, size_t placementCount = 0);

// The page identity drawForPage needs, for a screen that redraws the reader's page without the
// reader's Section (the dictionary word-select overlay).
struct PageKey {
  bool valid = false;
  uint16_t spineIndex = 0;
  float pageProgress = 0;
  int pageCount = 0;
  uint32_t chapterHash = 0;
  int pageNumber = 0;
  int markPageCount = 0;
};

// The token a page long-press landed on: the rect actually inked, so the caller can push just
// that region, and the token's own text for whatever names it on screen. `text` is
// NUL-terminated and cut on a codepoint boundary, so it is safe at a C-string boundary.
struct WordHit {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  char text[40] = {};
};

// Inverts the token whose tap box contains (x, y) — black band, word redrawn white, which is
// exactly what DictionaryWordSelectActivity shows for the same word — and reports it in `out`.
// Draws nothing and returns false when the point hits no token (a margin or an inter-word gap).
//
// The tap boxes are the ones extractWords derives, not the token widths drawForPage measures:
// the point is resolved a second time by the word-select screen this feedback precedes, and a
// box derived by a different rule would let the two disagree about a boundary tap and mark a
// different word than the one that then gets selected.
//
// Call with the page still in the framebuffer; the band is drawn over the glyphs already there.
bool invertWordAtPoint(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop, int x,
                       int y, WordHit& out);

// The looked-up word covering (x, y) on this page, or nullptr. Answers "is the word under the
// finger one this book has a flashcard for", which is the question the page's hold menu asks
// before offering to delete that card.
//
// Walks the page exactly as drawForPage does and drives the same LookupMarks::step, so a mark is
// reported here if and only if the underline was drawn for it -- including a CJK word, which is
// a RUN of one-character tokens and so cannot be recognised from the held token alone. Returns
// nullptr when SETTINGS.lookupUnderline is off: the table is empty then, and there is no mark on
// screen to act on.
//
// `chapterHash`, `pageNumber` and `pageCount` are the page key drawForPage takes, and must be the
// same values (`pageCount` here is drawForPage's `markPageCount`).
const LookupMarks::Mark* lookupMarkAtPoint(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft,
                                           int marginTop, int x, int y, uint32_t chapterHash, int pageNumber,
                                           int pageCount);

}  // namespace PageMarks
