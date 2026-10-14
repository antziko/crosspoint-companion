// Whole-word CJK selection for DictionaryWordSelectActivity: drives matcha-reader's
// WordSelectionScan the way its EpubReaderWordLookupActivity does (a burst around the cursor on
// open, then 40 ms slices while the screen is up) and merges the page's per-character tokens
// into the words it finds. See docs/matcha-sync.md.
#include <DictIndex.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>

#include <algorithm>

#include "DictionaryWordSelectActivity.h"
#include "MappedInputManager.h"
#include "activities/RenderLock.h"

namespace {

// Cells either side of the cursor's character the opening burst maps before the first frame: one
// lookup window, so the word under the cursor is whole however it lines up with the cursor.
constexpr size_t kNeighbourCells = WordSelectionScan::kMaxLookupChars;
// Longest the opening burst may hold the first frame, as matcha caps its own.
constexpr unsigned long kBurstCapMs = 1500;
constexpr uint32_t kBurstSliceMs = 50;
constexpr uint32_t kBackgroundSliceMs = 40;
// The background scan re-merges at most this often; a finished scan merges at once.
constexpr unsigned long kRemergeIntervalMs = 1000;

}  // namespace

int DictionaryWordSelectActivity::charLoOf(const int flat) const {
  if (spans_.empty()) return flat;
  return flat >= 0 && flat < static_cast<int>(spans_.size()) ? spans_[flat].lo : -1;
}

int DictionaryWordSelectActivity::charHiOf(const int flat) const {
  if (spans_.empty()) return flat;
  return flat >= 0 && flat < static_cast<int>(spans_.size()) ? spans_[flat].hi : -1;
}

int DictionaryWordSelectActivity::flatOfChar(const int charIdx) const {
  if (spans_.empty()) return charIdx;
  return CjkWordMerge::mergedIndexOf(spans_, charIdx);
}

void DictionaryWordSelectActivity::mapTokenCells() {
  tokenGlyphs_.clear();
  if (!scan_ || tokenCells_.size() != static_cast<size_t>(navigator.wordCount())) return;
  const size_t onPage = scan_->onPageGlyphCount();
  // Cells of each line in order, spaces left out (initFromPage inserts them between Latin words;
  // extractWords counted none). lineStart[l] indexes `cells` at line l's first cell.
  std::vector<uint16_t> cells;
  std::vector<uint16_t> lineStart;
  uint16_t lines = 0;
  for (size_t i = 0; i < onPage; i++) lines = std::max<uint16_t>(lines, scan_->allGlyphs[i].column + 1);
  if (!reserveNoThrow(cells, onPage) || !reserveNoThrow(lineStart, static_cast<size_t>(lines) + 1) ||
      !reserveNoThrow(tokenGlyphs_, tokenCells_.size())) {
    LOG_ERR("DWS", "OOM: scan cell map");
    return;
  }
  lineStart.assign(static_cast<size_t>(lines) + 1, 0);
  int lastLine = -1;
  for (size_t i = 0; i < onPage; i++) {
    const auto& g = scan_->allGlyphs[i];
    if (g.codepoint == ' ') continue;
    while (lastLine < static_cast<int>(g.column)) lineStart[++lastLine] = static_cast<uint16_t>(cells.size());
    cells.push_back(static_cast<uint16_t>(i));
  }
  while (lastLine < static_cast<int>(lines)) lineStart[++lastLine] = static_cast<uint16_t>(cells.size());

  for (const TokenCell& tc : tokenCells_) {
    CjkWordMerge::TokenGlyphs tg;
    if (tc.line < lines && tc.count > 0) {
      const size_t from = lineStart[tc.line] + tc.ordinal;
      const size_t lineEnd = lineStart[tc.line + 1];
      if (from < lineEnd) {
        // A line-final hyphen the scan dropped leaves the token one cell short; count what is there.
        size_t count = 1;
        while (count < tc.count && from + count < lineEnd && cells[from + count] == cells[from] + count) count++;
        tg.start = cells[from];
        tg.count = static_cast<uint8_t>(count);
      }
    }
    tokenGlyphs_.push_back(tg);
  }
}

void DictionaryWordSelectActivity::aimScanAtCursor() {
  if (!scan_ || scan_->isDone()) return;
  const int ch = charLoOf(navigator.getCurrentFlatIndex());
  if (ch < 0 || ch >= static_cast<int>(tokenGlyphs_.size())) return;
  const uint16_t cell = tokenGlyphs_[ch].start;
  if (cell == CjkWordMerge::TokenGlyphs::kNoGlyph || scan_->isGlyphMapped(cell)) return;
  // A window early, so a word that starts before the cursor's character and runs through it is
  // segmented whole: aimAtGlyph reads cells before its target only as context.
  scan_->aimAtGlyph(cell >= kNeighbourCells ? cell - kNeighbourCells : 0);
}

bool DictionaryWordSelectActivity::remergeWords(int cursorChar) {
  if (!scan_ || navigator.isEmpty()) return false;
  const size_t found = scan_->selectableGlyphs.size();
  std::vector<CjkWordMerge::ScanWord> scanWords;
  if (!reserveNoThrow(scanWords, found)) return false;
  for (size_t i = 0; i < found && i < scan_->selectToAllIdx.size(); i++) {
    const size_t cell = scan_->selectToAllIdx[i];
    if (cell >= CjkWordMerge::TokenGlyphs::kNoGlyph) continue;
    const uint8_t len = scan_->selectableGlyphs[i].matchLen;
    scanWords.push_back({static_cast<uint16_t>(cell), static_cast<uint8_t>(len == 0 ? 1 : len)});
  }
  // The merge runs over the list the navigator holds now, words already merged included: each
  // token's cells are the union of its characters' (contiguous by construction), and a new scan
  // word only ever joins tokens no earlier word took.
  const int n = navigator.wordCount();
  std::vector<CjkWordMerge::TokenGlyphs> glyphs;
  if (!reserveNoThrow(glyphs, n)) return false;
  for (int i = 0; i < n; i++) {
    const int lo = charLoOf(i);
    const int hi = charHiOf(i);
    CjkWordMerge::TokenGlyphs g;
    if (lo >= 0 && hi < static_cast<int>(tokenGlyphs_.size()) && tokenGlyphs_[lo].start != g.kNoGlyph) {
      g = tokenGlyphs_[lo];
      for (int c = lo + 1; c <= hi; c++) {
        if (tokenGlyphs_[c].start != g.start + g.count || g.count + tokenGlyphs_[c].count > 255) {
          g = {};
          break;
        }
        g.count += tokenGlyphs_[c].count;
      }
    }
    glyphs.push_back(g);
  }

  // The cursor stays on the character it was on, now inside its word.
  if (cursorChar < 0) cursorChar = charLoOf(navigator.getCurrentFlatIndex());
  WordSelectNavigator::WordInfo before{};
  const bool hadSelection = navigator.getSelected() != nullptr;
  if (hadSelection) before = *navigator.getSelected();
  {
    // The render task reads the word list while it draws, so it is taken, merged and put back
    // under the lock. The merge itself is a single pass over one page.
    RenderLock lock(*this);
    std::vector<WordSelectNavigator::WordInfo> inWords;
    std::vector<WordSelectNavigator::Row> inRows;
    std::string inPool;
    const int cursorBefore = navigator.getCurrentFlatIndex();
    navigator.takeWords(inWords, inRows, inPool);
    std::vector<WordSelectNavigator::WordInfo> words;
    std::vector<WordSelectNavigator::Row> rows;
    std::string pool;
    std::vector<CjkWordMerge::Span> spans;
    if (!CjkWordMerge::merge(inWords, inRows, inPool, glyphs, scan_->onPageGlyphCount(), scanWords, verticalCellPx_ > 0,
                             words, rows, pool, spans)) {
      navigator.replaceWords(std::move(inWords), std::move(inRows), std::move(inPool), cursorBefore);
      LOG_ERR("DWS", "Word merge skipped: heap");
      return false;
    }
    // Back to characters: the merge's spans index the list it was given.
    for (auto& sp : spans) sp = {static_cast<int16_t>(charLoOf(sp.lo)), static_cast<int16_t>(charHiOf(sp.hi))};
    inWords = {};
    inRows = {};
    inPool = {};
    const int cursor = std::max(0, CjkWordMerge::mergedIndexOf(spans, std::max(0, cursorChar)));
    navigator.replaceWords(std::move(words), std::move(rows), std::move(pool), cursor);
    spans_ = std::move(spans);
    autoLo_ = autoHi_ = -1;
    suppressLo_ = suppressHi_ = -1;
  }
  mergedScanWords_ = found;
  lastMergeMs_ = millis();
  const auto* after = navigator.getSelected();
  const bool boxChanged = !hadSelection || !after || after->screenX != before.screenX ||
                          after->screenY != before.screenY || after->width != before.width ||
                          after->continuationIndex >= 0 || after->continuationOf >= 0;
  if (gloss_) {
    loadGlossCards();
    gloss_->forFlatIdx = -1;
    gloss_->probeLo = gloss_->probeHi = -1;
  }
  if (boxChanged || gloss_) {
    requestUpdate();
  } else if (prevHighlightIdx_ >= 0) {
    // Same box on screen: the differential snapshot still matches it under its new index.
    prevHighlightIdx_ = navigator.getCurrentFlatIndex();
  }
  SdDebugLog::log("DWS", "merge words=%u scanned=%u/%u done=%d", static_cast<unsigned>(navigator.wordCount()),
                  static_cast<unsigned>(found), static_cast<unsigned>(scan_->onPageGlyphCount()),
                  static_cast<int>(scan_->isDone()));
  return true;
}

void DictionaryWordSelectActivity::startWordScan() {
  if (!scan_) return;
  if (navigator.isEmpty()) {
    scan_.reset();
    return;
  }
  if (verticalCellPx_ == 0) mapTokenCells();
  tokenCells_.clear();
  tokenCells_.shrink_to_fit();
  if (tokenGlyphs_.size() != static_cast<size_t>(navigator.wordCount())) {
    LOG_ERR("DWS", "Scan cell map missing (%u/%d); per-character selection", static_cast<unsigned>(tokenGlyphs_.size()),
            navigator.wordCount());
    scan_.reset();
    return;
  }
  const unsigned long t0 = millis();
  const bool cached = !scanCachePath_.empty() && scan_->tryLoadCache(scanCachePath_, scanSpine_, scanPage_);
  scanCacheSaved_ = cached;
  if (!cached) {
    const int ch = navigator.getCurrentFlatIndex();
    const uint16_t cell = ch >= 0 && ch < static_cast<int>(tokenGlyphs_.size()) ? tokenGlyphs_[ch].start
                                                                                : CjkWordMerge::TokenGlyphs::kNoGlyph;
    aimScanAtCursor();
    const size_t onPage = scan_->onPageGlyphCount();
    auto neighbourhoodMapped = [&]() {
      if (cell == CjkWordMerge::TokenGlyphs::kNoGlyph) return !scan_->selectableGlyphs.empty();
      const size_t from = cell >= kNeighbourCells ? cell - kNeighbourCells : 0;
      const size_t to = std::min(onPage, static_cast<size_t>(cell) + kNeighbourCells + 1);
      for (size_t i = from; i < to; i++) {
        if (!scan_->isGlyphMapped(i)) return false;
      }
      return true;
    };
    while (!scan_->isDone() && !neighbourhoodMapped() && millis() - t0 < kBurstCapMs) scan_->step(kBurstSliceMs);
  }

  // A range opened by pointing (HighlightRange) survives the swap, re-begun on its word.
  const int anchorChar = navigator.isMultiSelecting() ? charLoOf(navigator.getAnchorFlatIndex()) : -1;
  if (!remergeWords(navigator.getCurrentFlatIndex())) {  // nothing merged yet: a character index
    // No room for the merge: the page stays per character.
    scan_.reset();
    return;
  }
  if (anchorChar >= 0) navigator.beginMultiSelectAt(flatOfChar(anchorChar));
  SdDebugLog::log("DWS", "scan open cached=%d burst=%lums found=%u", static_cast<int>(cached), millis() - t0,
                  static_cast<unsigned>(scan_->selectableGlyphs.size()));
}

void DictionaryWordSelectActivity::stepWordScan() {
  if (!scan_) return;
  // Not while the render task draws: the font decompressor may be holding most of the largest
  // block for a moment (matcha's own condition).
  if (!scan_->isDone() && !RenderLock::peek()) scan_->step(kBackgroundSliceMs);
  const bool done = scan_->isDone();
  if (scan_->selectableGlyphs.size() != mergedScanWords_ && (done || millis() - lastMergeMs_ >= kRemergeIntervalMs)) {
    using B = MappedInputManager::Button;
    int tx = 0;
    int ty = 0;
    const bool userBusy = navigator.isMultiSelecting() || stepFramePending_.load() || mappedInput.isPressed(B::Back) ||
                          mappedInput.isPressed(B::Confirm) || mappedInput.isPressed(B::Left) ||
                          mappedInput.isPressed(B::Right) || mappedInput.isPressed(B::Up) ||
                          mappedInput.isPressed(B::Down) || mappedInput.isScreenTouchHeld(tx, ty);
    if (!userBusy) remergeWords();
  }
  if (done && scan_->selectableGlyphs.size() == mergedScanWords_) {
    if (!scanCacheSaved_ && !scanCachePath_.empty()) scan_->saveCache(scanCachePath_, scanSpine_, scanPage_, 0);
    scanCacheSaved_ = true;
    // Nothing left to merge: give the scan and the dictionary caches back. spans_ stays, it is
    // what maps highlights to characters.
    scan_.reset();
    tokenGlyphs_ = {};
    DictIndex::releaseCaches();
  }
}
