#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Back-navigation stack for chained dictionary lookups, stored compactly.
//
// Instead of owning a copy of each prior headword (a std::string per entry —
// multi-word phrase lookups make these heap-allocated and unbounded), each entry
// references the headword's position in the persisted lookup-history log by
// distance-from-newest, plus the page the user was reading. The owning activity
// resolves the actual word from the history log at that index when navigating
// back (which also re-derives the canonical title for stem/alt-form lookups).
//
// Three load-bearing invariants (documented because a naive impl gets them wrong):
//   1. Indices are distance-from-the-NEWEST history entry, so they stay valid
//      under front-eviction (oldest entries dropping out of the capped log).
//      An oldest=0 index would drift as the log evicts.
//   2. Stored indices are a NON-CONTIGUOUS subset of history positions. A
//      back-then-forward leaves the abandoned branch in the log as a gap
//      (back-nav neither logs nor removes). Example: chain A→B→C, back to B,
//      forward to D (a word NOT already in the log) ⇒ log [A,B,C,D], stack
//      [A@3, B@2], skipping the gap C@1. The stack tracks exact positions,
//      gaps and all.
//   3. Every write to the log moves the indices, and the log DEDUPLICATES
//      (LookupHistory.h): a word already present is moved to newest rather than
//      appended, so the log does not grow and only entries nearer than its old
//      slot shift. Writes the chain did not initiate move them too — a recorded
//      miss is one (DictionaryLookupController::handleLookupFailed). Hence onForward()
//      and onHistoryWrite() both take the written word's PRE-write index and
//      route through shifted(); nothing here may assume a plain append.
//
// Pure: no I/O, no SD, no settings — host-unit-testable. The activity owns the
// history-log resolution and the cap value.
class LookupChain {
 public:
  struct Entry {
    uint8_t histIndex = 0;  // distance-from-newest of the headword in the history log
    uint16_t page = 0;      // page the user was on when they chained away
  };

  // historyCap = live history capacity (SETTINGS.getLookupHistoryCapValue()).
  // Entries whose referenced word would fall outside the capped log are dropped,
  // guaranteeing every remaining index always resolves.
  void reset(int historyCap) {
    entries_.clear();
    currentHistIndex_ = -1;
    cap_ = historyCap;
  }

  // Distance-from-newest of the currently displayed word (0 = newest).
  // -1 means the current word is not in the history log (cannot be referenced).
  void setCurrentHistIndex(int idx) { currentHistIndex_ = idx; }
  int currentHistIndex() const { return currentHistIndex_; }

  bool empty() const { return entries_.empty(); }
  int depth() const { return static_cast<int>(entries_.size()); }
  const Entry& at(int i) const { return entries_[i]; }  // entries_[0] is the oldest (bottom)

  // Distance-from-newest an index becomes after a word lands at the newest end of
  // the log. `dupIndex` is that word's PRE-write distance, or -1 when it was not in
  // the log. A word already present is MOVED, so the log does not grow: only the
  // entries that were nearer than its old slot shift, and its own slot becomes the
  // newest (0). A genuinely new word shifts everything by one.
  static int shifted(int index, int dupIndex) {
    if (dupIndex < 0 || index < dupIndex) return index + 1;
    return index == dupIndex ? 0 : index;
  }

  // A line landed in the history log. Moves every stored index and the current one,
  // then drops entries whose word left the capped log. Call it directly for a write
  // the chain did not navigate for — a recorded miss is the common case; onForward()
  // routes its own write through it.
  void onHistoryWrite(int dupIndex) {
    for (auto& e : entries_) {
      const int moved = shifted(e.histIndex, dupIndex);
      e.histIndex = static_cast<uint8_t>(moved > 0xFF ? 0xFF : moved);
    }
    if (currentHistIndex_ >= 0) currentHistIndex_ = shifted(currentHistIndex_, dupIndex);
    trimEvicted();
  }

  // Forward navigation: leaving the current word (displayed on `page`) for a new one.
  // `appended` is true when the new word was actually written to the history log, and
  // `dupIndex` is its pre-write distance-from-newest (-1 = it was not in the log).
  // Both come straight from the write's LookupHistory::WriteResult so the chain
  // follows what the log really did instead of re-deriving it. Pushes a back-entry
  // for the word being left, then makes the new word current.
  void onForward(uint16_t page, bool appended, int dupIndex = -1) {
    // Outside the currentHistIndex_ guard below: the log moved for the stored entries
    // whether or not the word being LEFT can be referenced (it cannot when it was a
    // stopword or history recording was off).
    if (appended) onHistoryWrite(dupIndex);
    if (currentHistIndex_ >= 0) {
      Entry e;
      e.histIndex = static_cast<uint8_t>(currentHistIndex_);
      e.page = page;
      entries_.push_back(e);
      trimEvicted();
    }
    currentHistIndex_ = appended ? 0 : -1;
  }

  // Back navigation: pop the most recent back-entry. The caller resolves the headword
  // from the history log at the returned histIndex and, once that lookup actually
  // lands, calls setCurrentHistIndex() with it. Deliberately does NOT move the current
  // index: a back-navigation that never completes (miss dismissed, cancelled) has to
  // leave the chain exactly as it was — see unpop().
  Entry pop() {
    Entry e = entries_.back();
    entries_.pop_back();
    return e;
  }

  // Undo a pop() whose back-navigation never produced a definition. No re-index: the
  // log cannot have moved meanwhile, because a back-nav lookup never records history.
  void unpop(const Entry& e) { entries_.push_back(e); }

 private:
  // Drop entries whose referenced word has been evicted from the capped log, so every
  // remaining index always resolves. Scans all of them rather than the bottom run: a
  // re-looked-up word moves to the newest end, which can leave the stack unordered (the
  // entry naming that word becomes 0 while nearer ones grow past it), and a bottom-only
  // walk would then stop before an entry that has crossed the cap. Backwards, so the
  // erase cannot shift an index not yet visited.
  void trimEvicted() {
    for (size_t i = entries_.size(); i-- > 0;) {
      if (entries_[i].histIndex >= cap_) entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }

  std::vector<Entry> entries_;  // entries_.back() is the top of the back-stack
  int currentHistIndex_ = -1;
  int cap_ = 0;
};
