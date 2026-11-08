#pragma once

#include <cstddef>

class Page;
struct PageLink;

// IndexTerm over a laid-out Page: the bridge from a link's tap box to the words it covers.
namespace IndexTermPage {

// The words page.links[linkIndex] covers ("207"), space-joined into `out`. False when none.
bool linkLabel(const Page& page, size_t linkIndex, char* out, size_t cap);

// The index entry's term for page.links[linkIndex] ("Bob Hawke"), when the page reads as an
// index and the link is one of an entry's page references. False otherwise -- a footnote marker,
// a notes table, an ordinary cross-reference.
// `before`, when given, is the page ahead of it: its words are read first, for an entry that begins
// there. out receives "term", or "term" IndexTerm::kParentSeparator "heading" for a sub-entry.
bool termForLink(const Page& page, const Page* before, size_t linkIndex, char* out, size_t cap);

}  // namespace IndexTermPage
