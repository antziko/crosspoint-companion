#pragma once

#include <string>
#include <vector>

#include "VerticalClipCells.h"
#include "util/CjkWordMerge.h"
#include "util/WordSelectNavigator.h"

class GfxRenderer;

// One selectable unit of a vertical page, numbered in reading order. Word-select's tokens, saved
// highlights and the page marks all use this numbering. `text` is valid only during the visit.
struct VerticalToken {
  const VerticalClipCell& cell;
  const char* text = nullptr;
  size_t len = 0;
  bool isCjk = false;
  uint16_t index = 0;
};
// Return false to stop the walk.
using VerticalTokenVisitor = bool (*)(void* ctx, const VerticalToken& token);
// Visits every selectable cell (its character, or a whole Latin run) of `page`.
void forEachVerticalToken(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft,
                          int marginTop, int cellPx, void* ctx, VerticalTokenVisitor visit);

// The word-select token list for a vertical page, in the shape extractWords builds for a
// horizontal one: one token per selectable character cell (CJK text tokenises per character in
// both layouts) and one per Latin/digit run, each column a Row, columns right to left. A token's
// `width` is its extent down the column (WordSelectNavigator::setVertical).
// `scanCells`, when given, receives each token's cell in a WordSelectionScan built from the same
// page (initFromVerticalPage numbering); a rotated Latin run, which the scan skips, gets none.
// Returns false, leaving the outputs empty, when the heap cannot hold the list.
bool buildVerticalWordTokens(const GfxRenderer& renderer, const VerticalPage& page, int fontId, int marginLeft,
                             int marginTop, int cellPx, std::vector<WordSelectNavigator::WordInfo>& words,
                             std::vector<WordSelectNavigator::Row>& rows, std::string& textPool,
                             std::vector<CjkWordMerge::TokenGlyphs>* scanCells = nullptr);
