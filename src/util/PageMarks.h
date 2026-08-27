#pragma once

#include <cstdint>

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

}  // namespace PageMarks
