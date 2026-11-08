#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Back-of-book index entries ("Bob Hawke, 323"): telling a page-number link from a footnote
 * marker, and recovering the entry's term so a jump can land on the page that prints it.
 *
 * An EPUB index links each number to an anchor placed where its printed page begins, which on a
 * small screen can be several pages before the term itself. Pure functions over a flat token
 * list (the page's words in reading order) so the rules are host-testable without a Page.
 */
namespace IndexTerm {

constexpr size_t kMaxTerm = 64;  // bytes, NUL included
// An entry as carried from the index to the jump: "term", or "term" kParentSeparator "heading" for a
// sub-entry ("failure of" under "Apple III computer").
constexpr char kParentSeparator = '\x1F';
constexpr size_t kMaxEntry = 2 * kMaxTerm;
constexpr size_t kMaxCandidates = 6;
constexpr size_t kMinCandidateLen = 4;  // shorter text matches too much to move a page for

struct Token {
  const char* text;
  uint16_t len;
  uint16_t line;  // tokens sharing a value were laid out on one line
  int16_t x = 0;  // left edge on the page; a line's first word gives its indent
};

// A page reference: digits ("207"), a lowercase roman numeral ("xvii"), a range of either written
// as one token ("336-339", "336–339"), or one end of a digit range the layout split ("-47", "146-").
bool isPageRef(const char* text, size_t len);

// A link label of page references only, its words space-separated: "323", "146 -47", "307 -8,".
bool isPageRefLabel(const char* label);

// No letter or digit: ",", ";", "–", "(", ...
bool isSeparator(const char* text, size_t len);

// A page whose links read like an index: enough of them, nearly all page references.
bool looksLikeIndex(size_t linkCount, size_t pageRefLinks);

// The term of the entry whose page reference is tokens[linkToken], joined by single spaces into
// `out`. False when the link is not a page reference following "Term," -- a footnote marker in
// prose or a notes-table number has no comma-separated term before it.
// `firstOut`, when given, receives the index of the term's first token.
bool extractTerm(const Token* tokens, size_t count, size_t linkToken, char* out, size_t cap,
                 size_t* firstOut = nullptr);

// The heading a sub-entry starting at tokens[first] sits under: the nearest line indented less than
// the sub-entry's, or the line it runs on from. Its words up to the first comma or reference ("Apple
// III computer"). False for a top-level entry, or a heading that is not a capitalised name.
bool extractParent(const Token* tokens, size_t count, size_t first, char* out, size_t cap);

// Search strings for an entry, most specific first: the term without any parenthetical (an
// inverted name reordered, "Hawke, Bob" -> "Bob Hawke"), the parenthetical alone ("CPIB"), the
// opening two words of a longer term ("Apple I"), and one capitalised word -- the surname, or a
// sub-entry's longest name. Quotes and a sub-entry's trailing joining word ("media and") are
// dropped. Then, when `entry` carries a heading after kParentSeparator, the heading's phrase and
// opening words. Duplicates and strings shorter than kMinCandidateLen are dropped, except an
// abbreviation in capitals ("ANC") and a three-letter surname ("Ali, Muhammad"). Returns how many
// were written.
size_t buildCandidates(const char* entry, char (*out)[kMaxTerm], size_t maxOut);

}  // namespace IndexTerm
