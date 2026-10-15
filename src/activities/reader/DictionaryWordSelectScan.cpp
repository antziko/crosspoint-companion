// Whole-word CJK selection for DictionaryWordSelectActivity: drives matcha-reader's
// WordSelectionScan the way its EpubReaderWordLookupActivity does (a burst around the cursor on
// open, then 40 ms slices while the screen is up) and merges the page's per-character tokens
// into the words it finds. See docs/matcha-sync.md.
#include <DictIndex.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>

#include <algorithm>

#include "CrossPointSettings.h"
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
// A refused merge is retried once the largest block has grown this much, or after this long.
constexpr uint32_t kMergeRetryGrowth = 1024;
constexpr unsigned long kMergeRetryMs = 3000;
// A finished scan whose last merge has to wait is cut down to the word list the merge reads, so
// its glyph tables are freed at once. false keeps the whole scan until the merge lands.
constexpr bool kCompactFinishedScan = true;
// With the gloss box on, an uncached page skips the opening burst and the scan waits for the
// first frame: the page and the box get the heap before the dictionary caches (~30 KB) do, and
// words merge from the background steps a second or two later.
constexpr bool kDeferScanForGloss = true;
// The background scan steps aside while the user moves the cursor, and pauses when the dictionary
// caches could not be allocated (each probe would then read the SD card in full, a page took 28 s
// on device). A paused scan retries once the largest block has grown enough for the caches (the
// miss memo alone is 8 KB) or after a while.
constexpr bool kScanYields = true;
constexpr uint32_t kScanResumeGrowth = 8 * 1024;
constexpr unsigned long kScanRetryMs = 10000;
// The scan's caches can also leave the rest of the device starving without failing themselves: below
// both of these it pauses the same way. Both, because a scan with little free heap but whole blocks
// is healthy.
constexpr uint32_t kScanFloorFree = 10 * 1024;
constexpr uint32_t kScanFloorLargest = 4 * 1024;
// No scan slice this soon after a cursor step, so a run of presses never waits behind one.
constexpr unsigned long kScanQuietAfterStepMs = 1500;

// The scan's words as the merge takes them: first cell and length, in cells.
bool collectScanWords(const WordSelectionScan& scan, std::vector<CjkWordMerge::ScanWord>& out) {
  const size_t found = scan.selectableGlyphs.size();
  if (!reserveNoThrow(out, found)) return false;
  for (size_t i = 0; i < found && i < scan.selectToAllIdx.size(); i++) {
    const size_t cell = scan.selectToAllIdx[i];
    if (cell >= CjkWordMerge::TokenGlyphs::kNoGlyph) continue;
    const uint8_t len = scan.selectableGlyphs[i].matchLen;
    out.push_back({static_cast<uint16_t>(cell), static_cast<uint8_t>(len == 0 ? 1 : len)});
  }
  return true;
}

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
  if ((!scan_ && !scanCompacted_) || navigator.isEmpty()) return false;
  const size_t found = scan_ ? scan_->selectableGlyphs.size() : pendingFound_;
  const size_t onPage = scan_ ? scan_->onPageGlyphCount() : pendingOnPage_;
  std::vector<CjkWordMerge::ScanWord> collected;
  if (scan_ && !collectScanWords(*scan_, collected)) return false;
  const auto& scanWords = scan_ ? collected : pendingScanWords_;
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
  // The selection's band(s) on screen, both halves of a split word: a merge that leaves them where
  // they are changes nothing the reader can see, so it needs no repaint.
  struct Band {
    bool present = false;
    int16_t x = 0, y = 0, width = 0;
  };
  const auto bandsOf = [&](Band(&out)[2]) {
    out[0] = out[1] = {};
    const auto* w = navigator.getSelected();
    if (!w) return;
    out[0] = {true, w->screenX, w->screenY, w->width};
    const int partner = w->continuationIndex >= 0 ? w->continuationIndex : w->continuationOf;
    if (const auto* p = partner >= 0 ? navigator.getWordAt(partner) : nullptr) {
      out[1] = {true, p->screenX, p->screenY, p->width};
    }
  };
  Band before[2];
  bandsOf(before);
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
    if (!CjkWordMerge::merge(inWords, inRows, inPool, glyphs, onPage, scanWords, verticalCellPx_ > 0, words, rows, pool,
                             spans)) {
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
  Band after[2];
  bandsOf(after);
  const auto same = [](const Band& a, const Band& b) {
    return a.present == b.present && a.x == b.x && a.y == b.y && a.width == b.width;
  };
  const bool boxChanged =
      !before[0].present || !same(before[0], after[0]) || !same(before[1], after[1]) || navigator.isMultiSelecting();
  if (gloss_) {
    loadGlossCards();
    gloss_->probeLo = gloss_->probeHi = -1;
    // Same word under the cursor: its entry stands, under the index the merge gave it.
    gloss_->forFlatIdx = boxChanged ? -1 : navigator.getCurrentFlatIndex();
  }
  if (boxChanged) {
    requestUpdate();
  } else if (prevHighlightIdx_ >= 0) {
    // Same box on screen: the differential snapshot still matches it under its new index.
    prevHighlightIdx_ = navigator.getCurrentFlatIndex();
  }
  SdDebugLog::log("DWS", "merge words=%u scanned=%u/%u done=%d src=%s", static_cast<unsigned>(navigator.wordCount()),
                  static_cast<unsigned>(found), static_cast<unsigned>(onPage),
                  static_cast<int>(!scan_ || scan_->isDone()), scan_ ? "scan" : "list");
  return true;
}

void DictionaryWordSelectActivity::setGlossNote(const uint8_t note, const char* why) {
  if (glossNote_.exchange(note) == note) return;
  static constexpr const char* kNames[] = {"none", "working", "reopen"};
  SdDebugLog::log("DGL", "note=%s why=%s", note < 3 ? kNames[note] : "?", why);
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
    setGlossNote(kNoteReopen, "no cell map");
    return;
  }
  const unsigned long t0 = millis();
  const bool cached = !scanCachePath_.empty() && scan_->tryLoadCache(scanCachePath_, scanSpine_, scanPage_);
  scanCacheSaved_ = cached;
  scanDictReleased_ = false;
  scanIncomplete_ = false;
  setGlossNote(cached ? kNoteNone : kNoteWorking, cached ? "cached" : "scan start");
  mergeRefusedMs_ = 0;
  scanWaitsForFrame_ = false;
  scanPausedMs_ = 0;
  scanCompactTried_ = false;
  scanCompacted_ = false;
  pendingScanWords_ = {};
  if (!cached && kDeferScanForGloss && mode_ == Mode::Dictionary && SETTINGS.getReaderInlineGloss()) {
    scanWaitsForFrame_ = true;
    SdDebugLog::log("DWS", "scan open deferred (gloss) free=%u minEver=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    return;
  }
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
    while (!scan_->isDone() && !scan_->heapStalled() && !neighbourhoodMapped() && millis() - t0 < kBurstCapMs) {
      scan_->step(kBurstSliceMs);
    }
  }

  // A range opened by pointing (HighlightRange) survives the swap, re-begun on its word.
  const int anchorChar = navigator.isMultiSelecting() ? charLoOf(navigator.getAnchorFlatIndex()) : -1;
  if (!remergeWords(navigator.getCurrentFlatIndex())) {  // nothing merged yet: a character index
    // No room for the merge: the page stays per character.
    scan_.reset();
    setGlossNote(kNoteReopen, "first merge refused");
    return;
  }
  if (anchorChar >= 0) navigator.beginMultiSelectAt(flatOfChar(anchorChar));
  SdDebugLog::log("DWS", "scan open cached=%d burst=%lums found=%u free=%u minEver=%u", static_cast<int>(cached),
                  millis() - t0, static_cast<unsigned>(scan_->selectableGlyphs.size()), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap());
}

void DictionaryWordSelectActivity::compactFinishedScan() {
  scanCompactTried_ = true;
  // The cache is the scan's own result, independent of the merge, and needs the scan to write.
  if (!scanCacheSaved_ && !scanCachePath_.empty()) scan_->saveCache(scanCachePath_, scanSpine_, scanPage_, 0);
  scanCacheSaved_ = true;
  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t largestBefore = ESP.getMaxAllocHeap();
  if (!collectScanWords(*scan_, pendingScanWords_)) {
    pendingScanWords_ = {};
    SdDebugLog::log("DWS", "scan compact refused largest=%u", largestBefore);
    return;
  }
  pendingFound_ = scan_->selectableGlyphs.size();
  pendingOnPage_ = scan_->onPageGlyphCount();
  scan_.reset();
  scanCompacted_ = true;
  SdDebugLog::log("DWS", "scan compacted words=%u free=%u->%u largest=%u->%u",
                  static_cast<unsigned>(pendingScanWords_.size()), freeBefore, ESP.getFreeHeap(), largestBefore,
                  ESP.getMaxAllocHeap());
}

// One line per change of state: r run, f waiting for the first frame, p paused on the heap,
// b user busy, q quiet after a step, l render lock held, d done.
void DictionaryWordSelectActivity::logScanState(const char state) {
  if (state == scanState_) return;
  scanState_ = state;
  SdDebugLog::log("DWS", "scan state=%c pos=%u/%u found=%u free=%u largest=%u", state,
                  static_cast<unsigned>(scan_ ? scan_->scannedGlyphs() : 0),
                  static_cast<unsigned>(scan_ ? scan_->onPageGlyphCount() : pendingOnPage_),
                  static_cast<unsigned>(scan_ ? scan_->selectableGlyphs.size() : pendingFound_), ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
}

void DictionaryWordSelectActivity::stepWordScan() {
  if (!scan_ && !scanCompacted_) return;
  if (scanWaitsForFrame_) {
    if (!firstFrameShown_.load()) {
      logScanState('f');
      return;
    }
    scanWaitsForFrame_ = false;
    SdDebugLog::log("DWS", "scan starts after first frame free=%u minEver=%u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }
  using B = MappedInputManager::Button;
  int tx = 0;
  int ty = 0;
  const bool userBusy = navigator.isMultiSelecting() || stepFramePending_.load() || mappedInput.isPressed(B::Back) ||
                        mappedInput.isPressed(B::Confirm) || mappedInput.isPressed(B::Left) ||
                        mappedInput.isPressed(B::Right) || mappedInput.isPressed(B::Up) ||
                        mappedInput.isPressed(B::Down) || mappedInput.isScreenTouchHeld(tx, ty);
  bool stepNow = scan_ && !scan_->isDone();
  const bool quiet = lastStepMs_ != 0 && millis() - lastStepMs_ < kScanQuietAfterStepMs;
  char state = stepNow ? 'r' : 'd';
  if (kScanYields && stepNow) {
    if (scanPausedMs_ != 0) {
      if (ESP.getMaxAllocHeap() < scanPausedLargest_ + kScanResumeGrowth && millis() - scanPausedMs_ < kScanRetryMs) {
        stepNow = false;
        state = 'p';
      } else {
        scanPausedMs_ = 0;
        scan_->resumeAfterStall();
        SdDebugLog::log("DWS", "scan resumed free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
    } else if (const bool belowFloor =
                   scanCachesHeld_ && ESP.getFreeHeap() < kScanFloorFree && ESP.getMaxAllocHeap() < kScanFloorLargest;
               belowFloor || DictIndex::cachesStarved()) {
      const uint32_t freeBefore = ESP.getFreeHeap();
      DictIndex::releaseCaches();
      scanCachesHeld_ = false;
      scanPausedLargest_ = ESP.getMaxAllocHeap();
      scanPausedMs_ = std::max<unsigned long>(1, millis());
      stepNow = false;
      state = 'p';
      SdDebugLog::log("DWS", "scan paused (%s) free=%u->%u largest=%u minEver=%u", belowFloor ? "floor" : "heap",
                      freeBefore, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    }
    // Before a step can start, not after: one slice reads the SD card for tens of milliseconds.
    if (stepNow && (userBusy || quiet)) {
      stepNow = false;
      state = userBusy ? 'b' : 'q';
    }
  }
  // Not while the render task draws: the font decompressor may be holding most of the largest
  // block for a moment (matcha's own condition).
  if (stepNow && RenderLock::peek()) {
    stepNow = false;
    state = 'l';
  }
  logScanState(state);
  if (stepNow) {
    scan_->step(kBackgroundSliceMs);
    scanCachesHeld_ = true;
    if (scan_->heapStalled()) {
      // A found word could not be stored (the scan has released the caches): wait for the heap
      // like any other pause, then pick the walk up where it stalled.
      scanCachesHeld_ = false;
      scanPausedLargest_ = ESP.getMaxAllocHeap();
      scanPausedMs_ = std::max<unsigned long>(1, millis());
      SdDebugLog::log("DWS", "scan paused (stall) free=%u largest=%u minEver=%u", ESP.getFreeHeap(),
                      ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    }
  }
  const bool done = !scan_ || scan_->isDone();
  if (done && !scanDictReleased_) {
    // The finished scan reads no more dictionary; only the merge below is left, and it waits for
    // the user to pause. Free the caches now rather than holding them through a busy session.
    scanDictReleased_ = true;
    scanIncomplete_ = scan_ && scan_->wasTruncated();
    const uint32_t freeBefore = ESP.getFreeHeap();
    const uint32_t largestBefore = ESP.getMaxAllocHeap();
    DictIndex::releaseCaches();
    scanCachesHeld_ = false;
    SdDebugLog::log("DWS", "scan done, caches released free=%u->%u largest=%u->%u minEver=%u", freeBefore,
                    ESP.getFreeHeap(), largestBefore, ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
  }
  if (kCompactFinishedScan && scan_ && done && !scanCompactTried_ &&
      scan_->selectableGlyphs.size() != mergedScanWords_) {
    compactFinishedScan();
  }
  const size_t found = scan_ ? scan_->selectableGlyphs.size() : pendingFound_;
  if (found != mergedScanWords_ && (done || millis() - lastMergeMs_ >= kRemergeIntervalMs)) {
    // After a refused merge, wait for the heap to change rather than retrying every tick.
    const bool backingOff = mergeRefusedMs_ != 0 && ESP.getMaxAllocHeap() < mergeRefusedLargest_ + kMergeRetryGrowth &&
                            millis() - mergeRefusedMs_ < kMergeRetryMs;
    // Not within the quiet period either: a merge requests a repaint, which a press would wait behind.
    if (!userBusy && !quiet && !backingOff) {
      if (remergeWords()) {
        mergeRefusedMs_ = 0;
      } else {
        if (mergeRefusedMs_ == 0) {
          SdDebugLog::log("DWS", "merge refused largest=%u free=%u (backing off)", ESP.getMaxAllocHeap(),
                          ESP.getFreeHeap());
        }
        mergeRefusedLargest_ = ESP.getMaxAllocHeap();
        mergeRefusedMs_ = std::max<unsigned long>(1, millis());
      }
    }
  }
  if (done && found == mergedScanWords_) {
    if (scan_ && !scanCacheSaved_ && !scanCachePath_.empty()) {
      scan_->saveCache(scanCachePath_, scanSpine_, scanPage_, 0);
    }
    scanCacheSaved_ = true;
    // Nothing left to merge: give the scan and the dictionary caches back. spans_ stays, it is
    // what maps highlights to characters.
    scan_.reset();
    pendingScanWords_ = {};
    scanCompacted_ = false;
    tokenGlyphs_ = {};
    DictIndex::releaseCaches();
    scanCachesHeld_ = false;
  }
  const uint8_t note = scanIncomplete_ ? kNoteReopen : (scan_ || scanCompacted_) ? kNoteWorking : kNoteNone;
  setGlossNote(note, scanIncomplete_ ? "scan stopped" : note == kNoteNone ? "grouped" : "scan");
  // Outside a press, as a merge's repaint is; a press repaints anyway and picks the note up.
  if (gloss_ && gloss_->drawnY != kGlossNotDrawn && note != glossNoteDrawn_.load() && !userBusy && !quiet) {
    requestUpdate();
  }
}
