#pragma once

#include <cstdint>

class GfxRenderer;
class Page;

// Saved quotes ("highlights"), drawn onto the page they were taken from.
namespace QuoteHighlight {

// Marks every quote anchored on this page, in the coordinate space the page was just rendered
// in — a 25% dither band behind the words, or a rule under them, per SETTINGS.quoteHighlightStyle.
// Call after Page::render() on a black-and-white pass.
//
// A quote stores its selection as page-local word indices (BookmarkStore.h:60-64), which only
// exist as numbers — turning them back into pixels means walking the page's tokens in the same
// order DictionaryWordSelectActivity::extractWords numbered them, which is why both walks share
// PageTokenScan. The walk allocates nothing and is skipped entirely when no quote is anchored
// here, which is nearly every page.
void drawForPage(const GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                 uint16_t spineIndex, float pageProgress, int pageCount);

}  // namespace QuoteHighlight
