#pragma once

#include <cstdint>

#include "LookupMarks.h"

class GfxRenderer;
class Page;

// The marks the reader draws on top of a rendered page: saved quotes ("highlights") and the
// words looked up in the dictionary. Both are resolved by walking the page's tokens in the
// order PageTokens numbers them, so they share one walk and one set of measurements.
namespace PageMarks {

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
// LookupMarks), so `chapterHash` and the 1-based `pageNumber` identify the page it belongs to.
// The walk allocates nothing and is skipped entirely when neither kind of mark lands here,
// which is nearly every page.
void drawForPage(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                 uint16_t spineIndex, float pageProgress, int pageCount, uint32_t chapterHash, int pageNumber);

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
// same values -- a chapter that has re-paginated matches nothing, by design.
const LookupMarks::Mark* lookupMarkAtPoint(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft,
                                           int marginTop, int x, int y, uint32_t chapterHash, int pageNumber,
                                           int pageCount);

}  // namespace PageMarks
