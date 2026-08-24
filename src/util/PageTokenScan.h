#pragma once

#include <EpdFontFamily.h>

#include <cstddef>
#include <cstdint>
#include <string>

class GfxRenderer;

// The page word-index space, defined once.
//
// DictionaryWordSelectActivity::extractWords numbers the selectable tokens of a page, and
// BookmarkStore persists those numbers as a quote's startWord/endWord. The reader has to
// turn them back into pixels to underline a saved quote, which means walking the same page
// and arriving at the same numbers. The two rules that decide how many indices a layout
// word yields — whether it is selectable at all, and how a dash splits it — therefore live
// here rather than in either walk: if they ever disagree, every stored highlight silently
// shifts.
namespace PageTokens {

// True when the token carries at least one letter or digit in either script family, i.e.
// gets a cursor stop. outIsCjk reports whether it carries a CJK letter (Han, Kana, Hangul,
// fullwidth), which callers use to pick a measurement strategy — CJK tokenises one word per
// character, so a page of it produces ~400 tokens where English produces ~60.
//
// Takes pointer+length rather than std::string because the counting pass runs it over every
// token on a page before anything is allocated; a std::string per token would put heap
// traffic in front of the very reserve that count exists to protect.
bool isSelectable(const char* text, size_t len, bool& outIsCjk);

// Number of dash separators in the token: en-dash (U+2013), em-dash (U+2014), and any run
// of two or more ASCII hyphens, which is how a typewriter em-dash reaches an EPUB. Each one
// splits its token into an extra index, so a counting pass needs the same tally the
// extraction pass derives from the parts below. A token ending in a separator counts one
// more than it yields parts; callers use this to size a reservation, where erring high is
// free.
size_t countDashes(const char* text, size_t len);

// Cap on the parts one layout word can split into, sized for the stack arrays both walks
// declare rather than for any real token (dash-split words run ~0-2 per page, with one dash
// each). Both pass this same cap, so even a pathological token with more dashes than fit
// truncates identically on each and the index space stays consistent.
inline constexpr size_t kMaxTokenParts = 8;

// Byte range [start, end) of one emitted part within a layout word.
struct Part {
  uint16_t start;
  uint16_t end;
};

// Split a selectable token on its dash separators (see countDashes) into the parts that each
// become one word index. Separator bytes belong to no part, so "east--west" yields "east"
// and "west" and a trailing "word--" yields just "word" — which is also what stops the
// hyphenated-pair merge from mistaking that dash for a line-break hyphen.
// Writes at most maxParts entries and returns how many were written. A token containing no
// dash yields exactly one part spanning the whole token, which callers detect (and treat as
// the unsplit fast path) by checking for a single part covering [0, len).
size_t collectParts(const char* text, size_t len, Part* out, size_t maxParts);

// Advance width of a token, with soft hyphens (U+00AD) removed first. Layout strips them
// before measurement (ParsedText.cpp:19), so a measurement that kept them would overrun the
// word into the inter-word gap. The std::string form is the allocation-free one when the
// token is clean; the pointer+length form copies, and exists for callers holding a substring.
int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const std::string& word, EpdFontFamily::Style style);
// Whole-token form. `text` must be NUL-terminated — TextBlock's word arena is — and this is
// the one that copies nothing at all, which is why the quote underline measures through it.
int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style);
// Sub-range form, for a dash-split part or the prefix before one. Copies, since the range is
// not NUL-terminated.
int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const char* text, size_t len,
                       EpdFontFamily::Style style);

}  // namespace PageTokens
