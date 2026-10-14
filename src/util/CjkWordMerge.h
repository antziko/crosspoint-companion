#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/WordSelectNavigator.h"

// Turns word-select's per-character CJK tokens into whole words, using the words a
// WordSelectionScan segmented on the same page. Pure: no renderer, no SD.
namespace CjkWordMerge {

// The scan cells (WordSelectionScan::allGlyphs indices) a token covers. start == kNoGlyph for a
// token the scan does not see (a rotated Latin run on a vertical page).
struct TokenGlyphs {
  static constexpr uint16_t kNoGlyph = 0xFFFF;
  uint16_t start = kNoGlyph;
  uint8_t count = 0;
};

// One scan word: its first cell and its length in cells.
struct ScanWord {
  uint16_t start;
  uint8_t length;
};

// The original tokens an output token stands for, inclusive.
struct Span {
  int16_t lo;
  int16_t hi;
};

// Merges every run of tokens that exactly spells one scan word: the word starts on a token's
// first cell, the tokens are consecutive and cell-adjacent, and the last one reaches the word's
// end (trailing glued punctuation, 國。, may run past it). A word broken across two rows becomes
// two tokens linked as a continuation pair sharing the whole word's lookup text; one spanning
// more rows, and every token no word covers, is left as it is. `cellCount` bounds the cells a
// word may reach (the on-page count). Rows are rebuilt in the same order.
// `vertical`: a token's `width` runs down the column, so a merged box grows along screenY.
// Returns false, leaving the outputs empty, when the heap cannot hold them.
bool merge(const std::vector<WordSelectNavigator::WordInfo>& words, const std::vector<WordSelectNavigator::Row>& rows,
           const std::string& pool, const std::vector<TokenGlyphs>& glyphs, size_t cellCount,
           const std::vector<ScanWord>& scanWords, bool vertical, std::vector<WordSelectNavigator::WordInfo>& outWords,
           std::vector<WordSelectNavigator::Row>& outRows, std::string& outPool, std::vector<Span>& outSpans);

// Output token holding original token `orig`, or -1.
int mergedIndexOf(const std::vector<Span>& spans, int orig);

}  // namespace CjkWordMerge
