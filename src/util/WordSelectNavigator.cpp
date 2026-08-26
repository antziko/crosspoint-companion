#include "WordSelectNavigator.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>

#include "DictStopwords.h"
#include "MappedInputManager.h"
#include "TextPool.h"

void WordSelectNavigator::load(std::vector<WordInfo> w, std::vector<Row> r, std::string pool,
                               bool consumeInitialConfirm, InitialMarker initialMarker) {
  words = std::move(w);
  rows = std::move(r);
  textPool = std::move(pool);
  const int rowCount = static_cast<int>(rows.size());
  int targetRow;
  switch (initialMarker) {
    case InitialMarker::Top:
      targetRow = rowCount / 4;
      break;
    case InitialMarker::Bottom:
      targetRow = (rowCount * 3) / 4;
      break;
    case InitialMarker::Middle:
    default:
      targetRow = rowCount / 2;
      break;
  }
  currentRow = std::clamp(targetRow, 0, rowCount > 0 ? rowCount - 1 : 0);
  // Quote selection (skipStopwords_ off) keeps the plain index-middle: it has to be able to
  // start on "the" like any other word.
  currentWordInRow = 0;
  if (!rows.empty() && !rowEmpty(currentRow)) {
    const int middleOfRow = rowSize(currentRow) / 2;
    currentWordInRow = skipStopwords_ ? contentWordNear(currentRow, middleOfRow) : middleOfRow;
  }
  confirmReleaseConsumed = consumeInitialConfirm;
  // Seed the aim column from the page's text block, not from the starting row: a row that
  // holds a single short word would otherwise hand the first row move that word's own
  // far-left X, and because both row branches store what they aimed at, every move after it
  // would keep landing at the start of the line.
  rowNavGoalX = textBlockCenterX();
}

void WordSelectNavigator::organizeIntoRows(std::vector<WordInfo>& words, std::vector<Row>& rows) {
  if (words.empty()) return;
  int16_t currentY = words[0].screenY;
  rows.push_back({currentY, 0, 0});
  for (size_t i = 0; i < words.size(); i++) {
    if (std::abs(words[i].screenY - currentY) > 2) {
      currentY = words[i].screenY;
      rows.push_back({currentY, static_cast<int16_t>(i), 0});
    }
    words[i].row = static_cast<int16_t>(rows.size() - 1);
    rows.back().wordCount++;
  }
}

void WordSelectNavigator::mergeHyphenatedPairs(std::vector<WordInfo>& words, const std::vector<Row>& rows,
                                               std::string& textPool) {
  for (size_t r = 0; r + 1 < rows.size(); r++) {
    if (rows[r].wordCount == 0 || rows[r + 1].wordCount == 0) continue;

    int lastWordIdx = rows[r].firstWord + rows[r].wordCount - 1;
    const char* lastWord = textPool.data() + words[lastWordIdx].textOffset;
    uint16_t lastLen = words[lastWordIdx].textLen;
    if (lastLen == 0) continue;
    if (!utf8EndsWithHyphen(lastWord, lastLen)) continue;
    // A word that also starts with '-' (e.g. -re-) is a standalone affix token,
    // not the first half of a line-break compound.
    if (lastWord[0] == '-') continue;

    int nextWordIdx = rows[r + 1].firstWord;
    words[lastWordIdx].continuationIndex = static_cast<int16_t>(nextWordIdx);
    words[nextWordIdx].continuationOf = static_cast<int16_t>(lastWordIdx);

    std::string firstPart(lastWord, lastLen);
    utf8RemoveTrailingHyphen(firstPart);
    const char* nextWord = textPool.data() + words[nextWordIdx].textOffset;
    const char* strippedNext = (nextWord[0] == '-') ? nextWord + 1 : nextWord;
    std::string merged = firstPart + strippedNext;
    uint16_t mergedOff = poolAppend(textPool, merged.c_str(), merged.size());
    words[lastWordIdx].lookupOffset = mergedOff;
    words[lastWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
    words[nextWordIdx].lookupOffset = mergedOff;
    words[nextWordIdx].lookupLen = static_cast<uint16_t>(merged.size());
  }
}

uint16_t WordSelectNavigator::poolAppend(std::string& pool, const char* s, size_t len) {
  return TextPool::append(pool, s, len);
}

void WordSelectNavigator::reset() {
  words.clear();
  rows.clear();
  textPool.clear();
  currentRow = 0;
  currentWordInRow = 0;
  inMultiSelectMode = false;
  confirmReleaseConsumed = false;
  anchorFlatIndex = -1;
  pendingSnapIdx = -1;
  rowNavGoalX = -1;
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getSelected() const {
  if (rows.empty() || currentRow >= static_cast<int>(rows.size())) return nullptr;
  if (rowEmpty(currentRow)) return nullptr;
  return &words[wordAt(currentRow, currentWordInRow)];
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getPairedHalf() const {
  const WordInfo* sel = getSelected();
  if (!sel) return nullptr;
  const int wordIdx = wordAt(currentRow, currentWordInRow);
  int otherIdx = (sel->continuationOf >= 0) ? sel->continuationOf : -1;
  if (otherIdx < 0 && sel->continuationIndex >= 0 && sel->continuationIndex != wordIdx) {
    otherIdx = sel->continuationIndex;
  }
  if (otherIdx >= 0 && otherIdx < static_cast<int>(words.size())) {
    return &words[otherIdx];
  }
  return nullptr;
}

int WordSelectNavigator::getCurrentFlatIndex() const {
  if (rows.empty() || currentRow >= static_cast<int>(rows.size())) return -1;
  if (rowEmpty(currentRow)) return -1;
  return wordAt(currentRow, currentWordInRow);
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getWordAt(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(words.size())) return nullptr;
  return &words[idx];
}

std::string WordSelectNavigator::buildPhrase(int fromIdx, int toIdx) const {
  const int lo = std::min(fromIdx, toIdx);
  const int hi = std::max(fromIdx, toIdx);
  std::string phrase;
  // Skip index for a hyphenated pair's second half once its merged lookup text
  // has already been emitted via the first half, so the pair isn't duplicated.
  int skipIdx = -1;
  for (int i = lo; i <= hi; i++) {
    if (i == skipIdx) continue;
    const auto* w = getWordAt(i);
    if (!w) continue;
    // getLookup() returns the merged, hyphen-stripped text for a hyphenated
    // pair (e.g. "externity" for "exter-" + "nity"), matching the single-word
    // lookup path. For ordinary words it equals the display text.
    const char* lookup = getLookup(*w);
    // CJK words are written without spaces, so joining 中 + 国 with one produces "中 国",
    // which matches no headword. utf8NeedsSpaceBetween is the shared rule — Section's
    // page-text flattening reconstructs the same one.
    if (utf8NeedsSpaceBetween(phrase, lookup)) phrase += ' ';
    phrase += lookup;
    if (w->continuationIndex >= 0) skipIdx = static_cast<int>(w->continuationIndex);
  }
  return phrase;
}

bool WordSelectNavigator::isStopwordAt(int flatIdx) const {
  const WordInfo& w = words[flatIdx];
  return DictStopwords::isStopword(textPool.data() + w.lookupOffset, w.lookupLen);
}

int WordSelectNavigator::contentWordNear(int row, int pos) const {
  if (rowEmpty(row)) return 0;
  const int n = rowSize(row);
  pos = std::clamp(pos, 0, n - 1);
  if (!isStopwordAt(wordAt(row, pos))) return pos;
  // Walk outwards by index rather than by pixel: this only ever nudges the cursor off a
  // closed-class word it happened to land on, so the nearest neighbour in either direction
  // is the least surprising place to put it.
  for (int d = 1; d < n; d++) {
    if (pos - d >= 0 && !isStopwordAt(wordAt(row, pos - d))) return pos - d;
    if (pos + d < n && !isStopwordAt(wordAt(row, pos + d))) return pos + d;
  }
  return pos;  // the whole row is closed-class
}

int WordSelectNavigator::textBlockCenterX() const {
  if (words.empty()) return -1;
  int minX = INT_MAX;
  int maxX = INT_MIN;
  for (const auto& w : words) {
    minX = std::min<int>(minX, w.screenX);
    maxX = std::max<int>(maxX, w.screenX + w.width);
  }
  return (minX + maxX) / 2;
}

int WordSelectNavigator::findClosestWordFromX(int targetRow, int refCenterX, bool preferContentWord) const {
  if (rowEmpty(targetRow)) return 0;
  int bestMatch = 0;
  int bestDist = INT_MAX;
  int bestContent = -1;
  int bestContentDist = INT_MAX;
  for (int i = 0; i < rowSize(targetRow); i++) {
    const int flat = wordAt(targetRow, i);
    const int dist = std::abs(wordCenterX(flat) - refCenterX);
    if (dist < bestDist) {
      bestDist = dist;
      bestMatch = i;
    }
    if (preferContentWord && dist < bestContentDist && !isStopwordAt(flat)) {
      bestContentDist = dist;
      bestContent = i;
    }
  }
  return bestContent >= 0 ? bestContent : bestMatch;
}

void WordSelectNavigator::advanceHorizontal(const bool forward) {
  const int rowCount = static_cast<int>(rows.size());
  const int prevFlatIdx = getCurrentFlatIndex();

  if (forward) {
    if (currentWordInRow < rowSize(currentRow) - 1) {
      currentWordInRow++;
    } else if (rowCount > 1) {
      currentRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
      currentWordInRow = 0;
    } else {
      currentWordInRow = 0;  // single-row wrap
    }
  } else {
    if (currentWordInRow > 0) {
      currentWordInRow--;
    } else if (rowCount > 1) {
      currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
      currentWordInRow = rowSize(currentRow) - 1;
    }
  }

  // Hyphenated pair smoothing: the second half should not be a horizontal stop since both
  // halves highlight together. Row navigation is exempt — the user may intend to land on the
  // second half's row — so this lives here rather than in handleNavigation.
  const int idx = getCurrentFlatIndex();
  if (idx >= 0 && words[idx].continuationOf >= 0) {
    if (forward) {
      // Moving forward: skip past the second half to the next word.
      if (currentWordInRow < rowSize(currentRow) - 1) {
        currentWordInRow++;
      } else if (rowCount > 1) {
        currentRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
        currentWordInRow = 0;
      } else {
        currentWordInRow = 0;  // single-row wrap
      }
      // If the skip landed on yet another continuation, snap to its first half.
      const int skippedIdx = getCurrentFlatIndex();
      if (skippedIdx >= 0 && words[skippedIdx].continuationOf >= 0) {
        const int firstIdx = words[skippedIdx].continuationOf;
        currentRow = words[firstIdx].row;
        currentWordInRow = posInRow(currentRow, firstIdx);
      }
    } else {
      // Moving backward: snap to the first half.
      // Record the second half's index so subsequent row navigation
      // references its position rather than the first half's.
      pendingSnapIdx = idx;
      const int firstIdx = words[idx].continuationOf;
      currentRow = words[firstIdx].row;
      currentWordInRow = posInRow(currentRow, firstIdx);
    }
  }

  // Symmetric with the forward skip: if we came directly from the second half and wrapped
  // into its first half, skip backward past the first half so the pair is treated as a
  // single navigation unit in both directions.
  if (!forward) {
    const int curIdx = getCurrentFlatIndex();
    if (curIdx >= 0 && words[curIdx].continuationOf < 0 && words[curIdx].continuationIndex >= 0 &&
        prevFlatIdx == words[curIdx].continuationIndex) {
      if (currentWordInRow > 0) {
        currentWordInRow--;
      } else if (rowCount > 1) {
        currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
        currentWordInRow = rowSize(currentRow) - 1;
      }
    }
  }
}

bool WordSelectNavigator::handleNavigation(const MappedInputManager& input, const GfxRenderer& renderer,
                                           const bool swapAxes) {
  if (rows.empty()) return false;

  const auto orient = renderer.getOrientation();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const bool landscape = isLandscapeCw || isLandscapeCcw;

  bool rowPrevPressed, rowNextPressed, wordPrevPressed, wordNextPressed;

  if (isLandscapeCw) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Left, false);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Right, false);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Down);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Up);
  } else if (landscape) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Right, false);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Left, false);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Up);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Down);
  } else if (isInverted) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Down);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Up);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Right, false);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Left, false);
  } else {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Up);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Down);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Left, false);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Right, false);
  }

  // Trade the axes AFTER the orientation chain has resolved which physical button means
  // which direction, so each pair keeps its own direction sense in all four orientations
  // and the front reads keep their applySwap=false (this class does its own mapping).
  if (swapAxes) {
    std::swap(rowPrevPressed, wordPrevPressed);
    std::swap(rowNextPressed, wordNextPressed);
  }

  const int rowCount = static_cast<int>(rows.size());
  bool changed = false;

  // Row navigation aims at a reference column, freshest source first:
  //  - the second half a wordPrev just snapped away from (across rows), so rowPrev/rowNext
  //    feels like it originates from where the user was — this picks the base row too;
  //  - the column carried over from the last left/right step;
  //  - failing both, the cursor's own centre.
  // Any directional input clears the snap.
  const bool hasPendingSnap = pendingSnapIdx >= 0;
  const int rowNavBase = hasPendingSnap ? words[pendingSnapIdx].row : currentRow;
  int rowNavRefX;
  if (hasPendingSnap) {
    rowNavRefX = wordCenterX(pendingSnapIdx);
  } else if (rowNavGoalX >= 0) {
    rowNavRefX = rowNavGoalX;
  } else {
    rowNavRefX = rowEmpty(currentRow) ? 0 : wordCenterX(wordAt(currentRow, currentWordInRow));
  }
  if (rowPrevPressed || rowNextPressed || wordPrevPressed || wordNextPressed) {
    pendingSnapIdx = -1;
  }

  // Both row branches carry the reference column forward rather than the landed word's
  // own X, so passing through a row that holds a single short word leaves it intact.
  if (rowPrevPressed) {
    const int targetRow = (rowNavBase > 0) ? rowNavBase - 1 : rowCount - 1;
    currentWordInRow = findClosestWordFromX(targetRow, rowNavRefX, skipStopwords_ && !inMultiSelectMode);
    currentRow = targetRow;
    rowNavGoalX = rowNavRefX;
    changed = true;
  }

  if (rowNextPressed) {
    const int targetRow = (rowNavBase < rowCount - 1) ? rowNavBase + 1 : 0;
    currentWordInRow = findClosestWordFromX(targetRow, rowNavRefX, skipStopwords_ && !inMultiSelectMode);
    currentRow = targetRow;
    rowNavGoalX = rowNavRefX;
    changed = true;
  }

  if (wordPrevPressed) {
    advanceHorizontal(false);
    changed = true;
  }

  if (wordNextPressed) {
    advanceHorizontal(true);
    changed = true;
  }

  // Closed-class words are never what a lookup is after, so a left/right step keeps walking
  // until it reaches a content word. Bounded by the word count, and the landing position is
  // restored when the walk finds nothing else, so a page of nothing but stopwords still moves
  // exactly one stop per press instead of spinning or feeling dead. Multi-select is excluded
  // for the same reason row navigation excludes it: the range is contiguous, and a phrase like
  // "man of the world" has to stay selectable.
  if (skipStopwords_ && !inMultiSelectMode && (wordPrevPressed || wordNextPressed)) {
    const bool forward = wordNextPressed;
    const int landedRow = currentRow;
    const int landedWordInRow = currentWordInRow;
    const int landedSnapIdx = pendingSnapIdx;
    for (int guard = static_cast<int>(words.size()); guard > 0; guard--) {
      const int idx = getCurrentFlatIndex();
      if (idx < 0 || !isStopwordAt(idx)) break;
      advanceHorizontal(forward);
    }
    const int idx = getCurrentFlatIndex();
    if (idx >= 0 && isStopwordAt(idx)) {
      currentRow = landedRow;
      currentWordInRow = landedWordInRow;
      pendingSnapIdx = landedSnapIdx;
    }
  }

  // A left/right step redefines the column; the next row move re-derives it from wherever
  // the cursor ended up, hyphenated-pair smoothing included.
  if (wordPrevPressed || wordNextPressed) rowNavGoalX = -1;

  return changed;
}

WordSelectNavigator::MultiSelectAction WordSelectNavigator::handleMultiSelectInput(const MappedInputManager& input,
                                                                                   std::string& outPhrase,
                                                                                   unsigned long longPressMs) {
  if (inMultiSelectMode) {
    // Consume the Confirm release that follows the threshold-fire entry into multi-select.
    if (confirmReleaseConsumed) {
      if (input.wasReleased(MappedInputManager::Button::Confirm)) {
        confirmReleaseConsumed = false;
      }
      return MultiSelectAction::None;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      const int cursorIdx = getCurrentFlatIndex();
      outPhrase = buildPhrase(anchorFlatIndex, cursorIdx);
      inMultiSelectMode = false;
      return MultiSelectAction::PhraseReady;
    }
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      inMultiSelectMode = false;
      return MultiSelectAction::ExitedMultiSelect;
    }
    return MultiSelectAction::None;
  }

  // Consume the Confirm press+release that carried over from the long-press that opened word selection.
  // Must block both the held-state check (which would immediately enter multi-select) and
  // the subsequent release event (which would trigger a single-word lookup in the activity).
  if (confirmReleaseConsumed) {
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmReleaseConsumed = false;
    }
    return MultiSelectAction::Consumed;
  }

  // Long press Confirm: enter multi-select (fire at threshold, not on release).
  if (input.isPressed(MappedInputManager::Button::Confirm) && input.getHeldTime() >= longPressMs) {
    const int flatIdx = getCurrentFlatIndex();
    if (flatIdx >= 0) {
      inMultiSelectMode = true;
      anchorFlatIndex = flatIdx;
      confirmReleaseConsumed = true;
      return MultiSelectAction::EnteredMultiSelect;
    }
    return MultiSelectAction::Consumed;
  }

  return MultiSelectAction::None;
}

bool WordSelectNavigator::HighlightSnapshot::capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                                     const GfxRenderer& renderer) {
  if (w == 0 || h == 0) {
    bytes_ = 0;
    return false;
  }
  // The renderer translates (x, y, w, h) from screen to byte-aligned memory
  // coords and writes that many bytes; it returns 0 on capacity overflow,
  // out-of-bounds, or rejection. We do NOT pre-check capacity here because the
  // aligned-memory size differs from the naive screen-coord size, and only
  // the renderer knows the exact figure.
  const size_t written = renderer.readFramebufferRegion(x, y, w, h, buf_, MAX_SNAPSHOT_BYTES);
  if (written == 0) {
    bytes_ = 0;
    return false;
  }
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  bytes_ = written;
  return true;
}

void WordSelectNavigator::HighlightSnapshot::restore(GfxRenderer& renderer) const {
  if (bytes_ == 0) return;
  renderer.writeFramebufferRegion(x_, y_, w_, h_, buf_);
}

void WordSelectNavigator::renderHighlight(const GfxRenderer& renderer, int lineHeight) const {
  if (inMultiSelectMode) {
    const int cursorIdx = getCurrentFlatIndex();
    const int lo = std::min(anchorFlatIndex, cursorIdx);
    const int hi = std::max(anchorFlatIndex, cursorIdx);
    for (int i = lo; i <= hi; i++) {
      drawSingleHighlight(renderer, lineHeight, i);
      drawContinuationsIfOutside(renderer, lineHeight, getWordAt(i), lo, hi);
    }
  } else {
    const int selIdx = getCurrentFlatIndex();
    if (selIdx < 0) return;
    drawSingleHighlight(renderer, lineHeight, selIdx);
    drawContinuationsIfOutside(renderer, lineHeight, getWordAt(selIdx), selIdx, selIdx);
  }
}

void WordSelectNavigator::drawSingleHighlight(const GfxRenderer& renderer, int lineHeight, int wordIndex) const {
  const auto* w = getWordAt(wordIndex);
  if (!w) return;
  renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, true);
  renderer.drawText(fontIdFor(*w), w->screenX, w->screenY, getDisplay(*w), false, w->style);
}

void WordSelectNavigator::drawContinuationsIfOutside(const GfxRenderer& renderer, int lineHeight, const WordInfo* w,
                                                     int lo, int hi) const {
  if (!w) return;
  if (w->continuationIndex >= 0 && (w->continuationIndex < lo || w->continuationIndex > hi)) {
    drawSingleHighlight(renderer, lineHeight, w->continuationIndex);
  }
  if (w->continuationOf >= 0 && (w->continuationOf < lo || w->continuationOf > hi)) {
    drawSingleHighlight(renderer, lineHeight, w->continuationOf);
  }
}

WordSelectNavigator::Rect WordSelectNavigator::boundsForWord(int wordIndex, int lineHeight) const {
  const auto* w = getWordAt(wordIndex);
  if (!w) return Rect{};
  return Rect{static_cast<int>(w->screenX) - 2, static_cast<int>(w->screenY) - 2, static_cast<int>(w->width) + 4,
              lineHeight + 4};
}

WordSelectNavigator::Rect WordSelectNavigator::computeDirtyRect(int prevWordIdx, int currWordIdx,
                                                                int lineHeight) const {
  Rect curr = boundsForWord(currWordIdx, lineHeight);
  if (prevWordIdx < 0) return curr;
  Rect prev = boundsForWord(prevWordIdx, lineHeight);
  if (prev.width == 0 || prev.height == 0) return curr;
  if (curr.width == 0 || curr.height == 0) return prev;
  const int x0 = std::min(prev.x, curr.x);
  const int y0 = std::min(prev.y, curr.y);
  const int x1 = std::max(prev.x + prev.width, curr.x + curr.width);
  const int y1 = std::max(prev.y + prev.height, curr.y + curr.height);
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

std::optional<WordSelectNavigator::Rect> WordSelectNavigator::renderHighlightDifferential(GfxRenderer& renderer,
                                                                                          int lineHeight,
                                                                                          int prevWordIdx,
                                                                                          int currWordIdx) {
  // Fallback paths.
  if (inMultiSelectMode) return std::nullopt;
  const auto* curr = getWordAt(currWordIdx);
  if (!curr) return std::nullopt;
  if (curr->continuationIndex >= 0 || curr->continuationOf >= 0) {
    // Hyphenated wrap — two-word highlight is not yet supported by the
    // single-snapshot fast path. Caller falls back to full repaint.
    return std::nullopt;
  }

  // Step 1: restore pixels under the previous highlight (wipe it).
  // prevWordIdx < 0 is the caller's signal "no previous highlight on screen"
  // (typically because the framebuffer was just redrawn from scratch via the
  // full-repaint path or a sub-activity return). In that case any snapshot we
  // still hold from a prior render cycle is stale relative to the current
  // framebuffer — discard it rather than restoring it on top of fresh pixels.
  if (prevWordIdx >= 0 && snapshot_.valid()) {
    snapshot_.restore(renderer);
  }
  snapshot_.clear();

  // Step 2: snapshot pixels under the new highlight, clamping coordinates so we
  // never pass negative values into the renderer's uint16_t API.
  const Rect newRect = boundsForWord(currWordIdx, lineHeight);
  const uint16_t snapX = static_cast<uint16_t>(std::max(newRect.x, 0));
  const uint16_t snapY = static_cast<uint16_t>(std::max(newRect.y, 0));
  const uint16_t snapW = static_cast<uint16_t>(std::max(newRect.width, 0));
  const uint16_t snapH = static_cast<uint16_t>(std::max(newRect.height, 0));
  if (!snapshot_.capture(snapX, snapY, snapW, snapH, renderer)) {
    // Capture failed — either too big or out of bounds. Caller falls back.
    return std::nullopt;
  }

  // Step 3: draw the new highlight on top of the captured pixels.
  drawSingleHighlight(renderer, lineHeight, currWordIdx);

  // Step 4: caller pushes the union region.
  return computeDirtyRect(prevWordIdx, currWordIdx, lineHeight);
}
