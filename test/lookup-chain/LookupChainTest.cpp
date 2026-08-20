// Host-side litmus for LookupChain — the compact back-navigation stack.
//
// Verifies the three load-bearing invariants (distance-from-newest addressing;
// non-contiguous-subset under back-then-forward; every log write moves the indices,
// and the log DEDUPLICATES) against the design's worked example, plus eviction/
// depth-cap. Pure logic — no SD, no settings.
//
// The dedup cases run the chain against Log below — a faithful model of
// LookupHistory's file semantics — and assert on the WORD each entry resolves to,
// which is what the user sees. Index-only assertions cannot catch a shift that is
// self-consistent but wrong.
//
// Build/run: test/lookup-chain/run.sh

#include <cstdio>
#include <deque>
#include <string>

#include "util/LookupChain.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                                 \
  } while (0)

// Model of the on-SD history log, newest-first (index 0 = newest). Mirrors
// LookupHistory::addWordVer: a word already present is REMOVED and re-appended as
// newest (LookupHistory.h:11-13), so the log does not grow on a re-lookup.
class Log {
 public:
  explicit Log(int cap) : cap_(cap) {}
  // Returns the word's pre-write distance-from-newest, or -1 if it was not present —
  // the same value LookupHistory::WriteResult::prevIndex carries.
  int add(const std::string& word) {
    int prev = -1;
    for (size_t i = 0; i < words_.size(); ++i) {
      if (words_[i] == word) {
        prev = static_cast<int>(i);
        words_.erase(words_.begin() + static_cast<long>(i));
        break;
      }
    }
    words_.push_front(word);
    while (static_cast<int>(words_.size()) > cap_) words_.pop_back();
    return prev;
  }
  std::string at(int index) const {
    return index >= 0 && index < static_cast<int>(words_.size()) ? words_[static_cast<size_t>(index)] : "<none>";
  }

 private:
  std::deque<std::string> words_;
  int cap_;
};

// Resolve what Back would actually show, without consuming the entry.
static std::string backWord(const Log& log, const LookupChain& chain) {
  return chain.empty() ? "<empty>" : log.at(chain.at(chain.depth() - 1).histIndex);
}

// The design's worked example: A→B→C, back to B, forward to D.
// Expected log [A,B,C,D]; stack [A@3, B@2], skipping the abandoned gap C@1.
static void testWorkedTraceNonContiguousSubset() {
  std::printf("testWorkedTraceNonContiguousSubset\n");
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(0);  // viewing A, A is newest in the log

  // A -> B (B appended)
  c.onForward(/*page=*/0, /*appended=*/true);
  CHECK(c.depth() == 1, "after A->B: one entry");
  CHECK(c.at(0).histIndex == 1, "A at distance 1");
  CHECK(c.currentHistIndex() == 0, "B is newest");

  // B -> C (C appended)
  c.onForward(0, true);
  CHECK(c.depth() == 2, "after B->C: two entries");
  CHECK(c.at(0).histIndex == 2, "A at distance 2");
  CHECK(c.at(1).histIndex == 1, "B at distance 1");

  // back to B. pop() does not move the current index: the caller sets it only once the
  // re-lookup actually lands, so a back-nav that misses can be undone with unpop().
  LookupChain::Entry popped = c.pop();
  CHECK(popped.histIndex == 1, "popped B@1");
  CHECK(c.depth() == 1, "back leaves one entry");
  CHECK(c.at(0).histIndex == 2, "A still at distance 2");
  c.setCurrentHistIndex(popped.histIndex);
  CHECK(c.currentHistIndex() == 1, "now viewing B at distance 1");

  // B -> D (D appended) — the abandoned C stays in the log as a gap (C@1)
  c.onForward(0, true);
  CHECK(c.depth() == 2, "after B->D: two entries");
  CHECK(c.at(0).histIndex == 3, "A@3 (log [A,B,C,D])");
  CHECK(c.at(1).histIndex == 2, "B@2, skipping gap C@1");
  CHECK(c.currentHistIndex() == 0, "D is newest");
}

// Indices must address from the newest end so they survive front-eviction, and
// entries whose word leaves the capped log must be dropped (always-resolvable).
static void testEvictionDropsBottom() {
  std::printf("testEvictionDropsBottom\n");
  LookupChain c;
  c.reset(3);                // cap 3
  c.setCurrentHistIndex(0);  // viewing A, log [A]

  c.onForward(0, true);  // A->B: [A@1], log [A,B]
  c.onForward(0, true);  // B->C: [A@2,B@1], log [A,B,C] (full)
  CHECK(c.depth() == 2, "two entries before overflow");

  c.onForward(0, true);  // C->D: A would be evicted from log -> dropped from chain
  CHECK(c.depth() == 2, "depth stays bounded at eviction");
  CHECK(c.at(0).histIndex == 2, "B@2 now the bottom (A evicted)");
  CHECK(c.at(1).histIndex == 1, "C@1");
  CHECK(c.currentHistIndex() == 0, "D newest");
}

// Page is carried through forward->back.
static void testPageRoundTrip() {
  std::printf("testPageRoundTrip\n");
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(0);
  c.onForward(/*page=*/7, true);  // left a word on page 7
  LookupChain::Entry e = c.pop();
  CHECK(e.page == 7, "page restored");
}

// Current word not in the history log (-1) cannot be referenced: no entry pushed.
static void testUnloggedCurrentNotPushed() {
  std::printf("testUnloggedCurrentNotPushed\n");
  LookupChain c;
  c.reset(100);
  c.setCurrentHistIndex(-1);  // current word not in log
  c.onForward(0, true);       // can't reference it -> nothing pushed
  CHECK(c.empty(), "no entry pushed for an unreferenceable word");
  CHECK(c.currentHistIndex() == 0, "new word is newest");
}

// Re-looking up a word the log already holds MOVES it to newest instead of appending,
// so entries further from the newest end than its old slot must NOT shift. The regression:
// A→B→C, back to B, forward to C again — a +1-everything shift resolved Back to A.
static void testForwardToAbandonedBranchResolvesCorrectly() {
  std::printf("testForwardToAbandonedBranchResolvesCorrectly\n");
  Log log(100);
  LookupChain c;
  c.reset(100);
  log.add("A");
  c.setCurrentHistIndex(0);  // viewing A, freshly logged as the newest entry

  c.onForward(0, true, log.add("B"));
  c.onForward(0, true, log.add("C"));
  CHECK(backWord(log, c) == "B", "back from C is B");

  LookupChain::Entry popped = c.pop();  // back to B
  c.setCurrentHistIndex(popped.histIndex);
  CHECK(log.at(popped.histIndex) == "B", "landed on B");

  c.onForward(0, true, log.add("C"));  // forward to the abandoned branch
  CHECK(backWord(log, c) == "B", "back from the re-looked-up C is still B");
}

// A definition that links to its own headword: the log is unchanged (the word is already
// newest), so the entry pushed for it must stay at distance 0, not slide to 1.
static void testReLookupOfCurrentWord() {
  std::printf("testReLookupOfCurrentWord\n");
  Log log(100);
  LookupChain c;
  c.reset(100);
  log.add("X");
  c.setCurrentHistIndex(0);
  c.onForward(0, true, log.add("A"));
  c.onForward(0, true, log.add("A"));  // look up the word already on screen
  CHECK(backWord(log, c) == "A", "back returns A, not the word before it");
}

// A recorded miss (DictionaryLookupController::handleLookupFailed) writes to the log with no
// navigation. Un-re-indexed, every stored entry resolves one slot too new.
static void testRecordedMissReIndexes() {
  std::printf("testRecordedMissReIndexes\n");
  Log log(100);
  LookupChain c;
  c.reset(100);
  log.add("A");
  c.setCurrentHistIndex(0);
  c.onForward(0, true, log.add("B"));
  CHECK(backWord(log, c) == "A", "back from B is A");

  c.onHistoryWrite(log.add("nosuchword"));  // miss recorded while viewing B
  CHECK(backWord(log, c) == "A", "back is still A after a recorded miss");
  CHECK(c.currentHistIndex() == 1, "the word on screen moved to distance 1");

  c.onForward(0, true, log.add("D"));  // forward again from B
  CHECK(backWord(log, c) == "B", "back from D is B");
}

// A back-navigation that never produced a definition must leave the chain untouched:
// the entry is popped before the re-lookup starts.
static void testUnpopRestoresTheLevel() {
  std::printf("testUnpopRestoresTheLevel\n");
  Log log(100);
  LookupChain c;
  c.reset(100);
  log.add("A");
  c.setCurrentHistIndex(0);
  c.onForward(/*page=*/4, true, log.add("B"));

  LookupChain::Entry popped = c.pop();
  c.unpop(popped);  // the re-lookup missed / was cancelled
  CHECK(c.depth() == 1, "level restored");
  CHECK(c.at(0).histIndex == popped.histIndex, "index restored");
  CHECK(c.at(0).page == 4, "page restored");
  CHECK(c.currentHistIndex() == 0, "still viewing B, which is still newest");
  CHECK(backWord(log, c) == "A", "back still resolves to A");
}

// The word being left has no history slot (a stopword, or recording off), but the new
// word IS logged: the stored entries still have to move, even though none is pushed for
// the unreferenceable word itself. Before, the shift sat inside the "is the current word
// referenceable?" guard, so this trace left every entry pointing one slot too new — Back
// resolved to the word already on screen.
static void testUnloggedCurrentStillShiftsEntries() {
  std::printf("testUnloggedCurrentStillShiftsEntries\n");
  Log log(100);
  LookupChain c;
  c.reset(100);
  log.add("A");
  c.setCurrentHistIndex(0);
  c.onForward(0, true, log.add("B"));  // stack [A@1], viewing B
  c.onForward(0, false, -1);           // -> a stopword: nothing logged, no slot
  CHECK(c.currentHistIndex() == -1, "stopword has no history slot");
  c.onForward(0, true, log.add("D"));  // logged: the log grew under the stack
  CHECK(c.depth() == 2, "the stopword pushed nothing of its own");
  // The stopword level is unrecoverable by design (no history slot to name it), so Back
  // skips it and lands on the word before it.
  CHECK(backWord(log, c) == "B", "back from D resolves to B, not the word on screen");
  CHECK(log.at(c.at(0).histIndex) == "A", "the bottom entry still resolves to A");
}

// A re-looked-up word moves to the newest end, so the stack stops being ordered: the
// entry naming that word drops to 0 while nearer ones grow past it. Eviction must then
// still find the entry that crossed the cap, wherever it sits — a bottom-only walk stops
// at the small index now sitting at the bottom and leaves an index nothing can resolve.
static void testEvictionAfterAReorderingReLookup() {
  std::printf("testEvictionAfterAReorderingReLookup\n");
  Log log(4);
  LookupChain c;
  c.reset(4);
  log.add("A");
  c.setCurrentHistIndex(0);
  c.onForward(0, true, log.add("B"));
  c.onForward(0, true, log.add("C"));
  c.onForward(0, true, log.add("D"));  // stack [A@3, B@2, C@1], viewing D, log full

  c.onForward(0, true, log.add("A"));  // re-look-up A: it jumps to newest, stack unordered
  CHECK(c.depth() == 4, "still four levels");
  CHECK(backWord(log, c) == "D", "back from the re-looked-up A is D");

  c.onForward(0, true, log.add("E"));  // E appended -> B falls out of the capped log
  for (int i = 0; i < c.depth(); ++i) {
    CHECK(log.at(c.at(i).histIndex) != "<none>", "every surviving entry still resolves");
    CHECK(log.at(c.at(i).histIndex) != "B", "the evicted word's entry was dropped");
  }
  CHECK(backWord(log, c) == "A", "back from E is A");
}

int main() {
  std::printf("=== LookupChain host litmus ===\n");
  testWorkedTraceNonContiguousSubset();
  testEvictionDropsBottom();
  testPageRoundTrip();
  testUnloggedCurrentNotPushed();
  testForwardToAbandonedBranchResolvesCorrectly();
  testReLookupOfCurrentWord();
  testRecordedMissReIndexes();
  testUnpopRestoresTheLevel();
  testUnloggedCurrentStillShiftsEntries();
  testEvictionAfterAReorderingReLookup();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
