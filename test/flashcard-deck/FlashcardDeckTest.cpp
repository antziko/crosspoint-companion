#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "util/FlashcardDeck.h"

namespace {

using SessionScope = FlashcardDeck::SessionScope;

// Per-test scratch directory acting as a book cache dir.
class FlashcardDeckTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/fcdeck_XXXXXX";
    char* d = mkdtemp(tmpl);
    ASSERT_NE(d, nullptr);
    cachePath = d;
  }
  void TearDown() override {
    cleanupDir(cachePath);
    std::remove(cachePath.c_str());
    for (const auto& d : extraDirs) {
      cleanupDir(d);
      std::remove(d.c_str());
    }
  }

  // Create a second device's cache dir (cleaned up in TearDown).
  std::string makeDevice() {
    char tmpl[] = "/tmp/fcdeck_XXXXXX";
    char* d = mkdtemp(tmpl);
    EXPECT_NE(d, nullptr);
    extraDirs.emplace_back(d);
    return extraDirs.back();
  }

  // Find a card by word in `dir` (whole-deck scan). Returns false if absent.
  static bool findCard(const std::string& dir, const std::string& word, FlashcardDeck::Entry& out) {
    const int n = FlashcardDeck::count(dir);
    if (n == 0) return false;
    std::vector<FlashcardDeck::Entry> v(static_cast<size_t>(n));
    FlashcardDeck::loadWindow(dir, 0, n, v.data());
    for (auto& e : v)
      if (e.word == word) {
        out = e;
        return true;
      }
    return false;
  }

  // One A->B sync round: serialize A's upload blob, merge it into B, commit A.
  // Returns the merge result (cards added). `cap` bounds the per-sync slice.
  static int syncRound(const std::string& A, const std::string& B, size_t cap = 4096, uint32_t devices = 1,
                       int* outDeleted = nullptr) {
    std::vector<uint8_t> buf(cap);
    FlashcardDeck::BlobStats st;
    const size_t n = FlashcardDeck::serializeForUpload(A, buf.data(), cap, &st);
    int added = 0;
    if (n) added = FlashcardDeck::mergeBlob(B, buf.data(), n, outDeleted);
    FlashcardDeck::commitUpload(A, st, devices);
    return added;
  }

  // Remove the deck + all sync sidecars + temp files from a cache dir.
  static void cleanupDir(const std::string& dir) {
    for (const char* f :
         {"dictionary_flashcards.txt", "dictionary_flashcards.tmp", "dictionary_flashcards.ver",
          "dictionary_flashcards.tomb", "dictionary_flashcards.tomb.tmp", "dictionary_flashcards.sync"}) {
      std::remove((dir + "/" + f).c_str());
    }
  }

  // Read the card for a newest-first index (helper around loadWindow).
  FlashcardDeck::Entry at(int newestIdx) {
    FlashcardDeck::Entry e;
    EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, newestIdx, 1, &e), 1);
    return e;
  }

  std::string cachePath;
  std::vector<std::string> extraDirs;
};

// --------------------------------------------------------------------------
// Leitner core (pure, no I/O)
// --------------------------------------------------------------------------

// The full clean run for a card that was NOT known on sight: new -> +3d -> +7d -> mastered.
// Three correct recalls over ten days.
TEST_F(FlashcardDeckTest, GradeTablePromotesAndSchedules) {
  uint8_t box = 0;
  uint32_t due = 0;
  const uint32_t today = 100;

  // Missing the first showing is what puts the card on the ordinary one-rung-at-a-time
  // path; the first-showing fast track is covered separately below.
  FlashcardDeck::applyGrade(box, due, /*correct=*/false, today);
  ASSERT_EQ(box, 0);
  ASSERT_EQ(due, today + 1u);

  FlashcardDeck::applyGrade(box, due, /*correct=*/true, today);
  EXPECT_EQ(box, 1);
  EXPECT_EQ(due, today + 3u);

  FlashcardDeck::applyGrade(box, due, /*correct=*/true, today);
  EXPECT_EQ(box, FlashcardDeck::TOP_BOX);
  EXPECT_EQ(due, today + 7u);

  // Correct at the top box graduates; dueDay left untouched.
  const uint32_t dueBefore = due;
  FlashcardDeck::applyGrade(box, due, true, today);
  EXPECT_EQ(box, FlashcardDeck::RETIRED);
  EXPECT_EQ(due, dueBefore);
  EXPECT_TRUE(FlashcardDeck::isMastered(box));
}

// A word the reader already knew: right on the very first showing skips a rung, so it
// masters in two recalls instead of three. Graduation still costs a real interval.
TEST_F(FlashcardDeckTest, FirstShowingCorrectSkipsARung) {
  uint8_t box = 0;
  uint32_t due = 0;  // never scheduled == first showing
  const uint32_t today = 100;

  FlashcardDeck::applyGrade(box, due, /*correct=*/true, today);
  EXPECT_EQ(box, FlashcardDeck::TOP_BOX);
  EXPECT_EQ(due, today + 7u);
  EXPECT_FALSE(FlashcardDeck::isMastered(box));  // never straight to mastered

  FlashcardDeck::applyGrade(box, due, /*correct=*/true, today + 7);
  EXPECT_EQ(box, FlashcardDeck::RETIRED);
}

// The fast track is keyed on "never scheduled", not on "box 0": a card that lapsed back to
// box 0 has a real dueDay, and must climb one rung at a time like any other.
TEST_F(FlashcardDeckTest, RelearningCardDoesNotGetTheFirstShowingSkip) {
  uint8_t box = 1;
  uint32_t due = 200;
  const uint32_t today = 100;

  FlashcardDeck::applyGrade(box, due, /*correct=*/false, today);
  ASSERT_EQ(box, 0);
  ASSERT_NE(due, 0u);  // scheduled, so no longer a first showing

  FlashcardDeck::applyGrade(box, due, /*correct=*/true, today);
  EXPECT_EQ(box, 1);  // one rung, not two
  EXPECT_EQ(due, today + 3u);
}

// A miss costs one rung, not the whole run -- on a three-step ladder a reset would make a
// single lapse as expensive as never having studied the word.
TEST_F(FlashcardDeckTest, GradeMissDemotesOneBoxWithRelearnInterval) {
  uint8_t box = FlashcardDeck::TOP_BOX;
  uint32_t due = 200;
  const uint32_t today = 100;

  FlashcardDeck::applyGrade(box, due, /*correct=*/false, today);
  EXPECT_EQ(box, FlashcardDeck::TOP_BOX - 1);
  EXPECT_EQ(due, today + FlashcardDeck::BOX_INTERVAL_DAYS[FlashcardDeck::TOP_BOX - 1]);

  // Box 0 has nowhere to fall to; the 1-day relearn interval is what a lapse there buys.
  box = 0;
  FlashcardDeck::applyGrade(box, due, /*correct=*/false, today);
  EXPECT_EQ(box, 0);
  EXPECT_EQ(due, today + FlashcardDeck::BOX_INTERVAL_DAYS[0]);  // relearn = +1
}

// Decks written by the older six-box ladder can hold boxes above TOP_BOX. Those reps are
// already more than the current schedule asks for, so the next correct recall graduates.
TEST_F(FlashcardDeckTest, LegacyOverRangeBoxClampsToTopOfLadder) {
  uint8_t box = 4;  // impossible under the current ladder
  uint32_t due = 200;
  const uint32_t today = 100;

  FlashcardDeck::applyGrade(box, due, /*correct=*/true, today);
  EXPECT_EQ(box, FlashcardDeck::RETIRED);

  box = 5;
  FlashcardDeck::applyGrade(box, due, /*correct=*/false, today);
  EXPECT_EQ(box, FlashcardDeck::TOP_BOX - 1);  // clamped to TOP_BOX, then demoted one
}

// Neither sentinel is ever scheduled, so grading one is a caller bug. It must not be read
// as a box number -- a clamp would silently turn 255 into a live, due card.
TEST_F(FlashcardDeckTest, GradingASentinelBoxIsANoOp) {
  uint32_t due = 200;
  uint8_t box = FlashcardDeck::RETIRED;
  FlashcardDeck::applyGrade(box, due, /*correct=*/false, 100);
  EXPECT_EQ(box, FlashcardDeck::RETIRED);
  EXPECT_EQ(due, 200u);

  box = FlashcardDeck::SUSPENDED;
  FlashcardDeck::applyGrade(box, due, /*correct=*/true, 100);
  EXPECT_EQ(box, FlashcardDeck::SUSPENDED);
  EXPECT_EQ(due, 200u);
}

TEST_F(FlashcardDeckTest, IsDueSemantics) {
  EXPECT_TRUE(FlashcardDeck::isDue(0, 0, 50));                        // new (dueDay==0) always due
  EXPECT_TRUE(FlashcardDeck::isDue(2, 50, 50));                       // due today
  EXPECT_TRUE(FlashcardDeck::isDue(2, 40, 50));                       // overdue
  EXPECT_FALSE(FlashcardDeck::isDue(2, 60, 50));                      // future
  EXPECT_FALSE(FlashcardDeck::isDue(FlashcardDeck::RETIRED, 0, 50));  // retired never due
}

// --------------------------------------------------------------------------
// enroll / count / dedup / excerpt
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, EnrollAppendsAndCounts) {
  EXPECT_EQ(FlashcardDeck::count(cachePath), 0);
  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "alpha", "the alpha wolf"));
  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "beta", "a beta test"));
  EXPECT_EQ(FlashcardDeck::count(cachePath), 2);

  FlashcardDeck::Entry e = at(0);  // newest = beta
  EXPECT_EQ(e.word, "beta");
  EXPECT_EQ(e.box, 0);
  EXPECT_EQ(e.dueDay, 0u);
  EXPECT_EQ(e.excerpt, "a beta test");
}

TEST_F(FlashcardDeckTest, ReEnrollDedupsAndMovesToNewest) {
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx");
  FlashcardDeck::enroll(cachePath, "beta", "beta ctx");
  FlashcardDeck::enroll(cachePath, "alpha", "second ctx");  // dedup + refresh
  EXPECT_EQ(FlashcardDeck::count(cachePath), 2);
  EXPECT_EQ(at(0).word, "alpha");  // moved to newest
  EXPECT_EQ(at(0).excerpt, "second ctx");
  EXPECT_EQ(at(1).word, "beta");
}

TEST_F(FlashcardDeckTest, ReEnrollEmptyExcerptPreservesOriginal) {
  FlashcardDeck::enroll(cachePath, "alpha", "original sentence");
  FlashcardDeck::enroll(cachePath, "alpha", "");  // re-lookup from history list
  EXPECT_EQ(FlashcardDeck::count(cachePath), 1);
  EXPECT_EQ(at(0).excerpt, "original sentence");
}

TEST_F(FlashcardDeckTest, LookupCountIncrementsOnReEnroll) {
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx");
  EXPECT_EQ(at(0).count, 1u);  // brand new
  FlashcardDeck::enroll(cachePath, "alpha", "second ctx");
  EXPECT_EQ(at(0).count, 2u);                     // re-lookup bumps
  FlashcardDeck::enroll(cachePath, "alpha", "");  // empty re-lookup still counts
  EXPECT_EQ(at(0).count, 3u);
  EXPECT_EQ(at(0).excerpt, "second ctx");  // empty excerpt preserved old, count still bumped
}

// Enrollment is automatic on every lookup, so a re-lookup is not a failed recall: the card
// keeps its box and due day. Only grading it wrong in review demotes a card. Matches what
// updateRemoteCard already does when the same re-enroll arrives from a peer.
TEST_F(FlashcardDeckTest, ReEnrollKeepsTheReviewSchedule) {
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx");
  // One correct recall on the first showing puts the card on the top box with a real due
  // day -- mid-ladder, not graduated, which is what this test needs to survive a re-enroll.
  FlashcardDeck::grade(cachePath, "alpha", /*correct=*/true, /*today=*/10);
  FlashcardDeck::Entry before;
  ASSERT_TRUE(findCard(cachePath, "alpha", before));
  ASSERT_EQ(before.box, FlashcardDeck::TOP_BOX);
  ASSERT_GT(before.dueDay, 0u);

  FlashcardDeck::enroll(cachePath, "alpha", "second ctx");
  FlashcardDeck::Entry after;
  ASSERT_TRUE(findCard(cachePath, "alpha", after));
  EXPECT_EQ(after.box, before.box);  // schedule survives
  EXPECT_EQ(after.dueDay, before.dueDay);
  EXPECT_EQ(after.count, 2u);                // the lookup is still recorded
  EXPECT_GT(after.version, before.version);  // and still propagates
  EXPECT_EQ(after.excerpt, "second ctx");
}

// --------------------------------------------------------------------------
// re-count cooldown
// --------------------------------------------------------------------------

// A re-lookup inside the window leaves the card completely alone — no count bump, and no
// deck rewrite (the whole point: the rewrite is the expensive half).
TEST_F(FlashcardDeckTest, ReEnrollInsideTheWindowIsANoOp) {
  FlashcardDeck::clearEnrollCooldown();
  constexpr uint32_t kWindow = 5 * 60 * 1000;
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx", "", 0, /*nowMs=*/1000, kWindow);
  const uint32_t version = at(0).version;

  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "alpha", "second ctx", "", 0, /*nowMs=*/60000, kWindow));
  EXPECT_EQ(at(0).count, 1u);
  EXPECT_EQ(at(0).version, version);      // nothing was written, so nothing propagates
  EXPECT_EQ(at(0).excerpt, "first ctx");  // untouched, not refreshed
}

TEST_F(FlashcardDeckTest, ReEnrollPastTheWindowCountsAgain) {
  FlashcardDeck::clearEnrollCooldown();
  constexpr uint32_t kWindow = 5 * 60 * 1000;
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx", "", 0, /*nowMs=*/1000, kWindow);
  FlashcardDeck::enroll(cachePath, "alpha", "second ctx", "", 0, /*nowMs=*/1000 + kWindow, kWindow);
  EXPECT_EQ(at(0).count, 2u);
  EXPECT_EQ(at(0).excerpt, "second ctx");
}

// The window is keyed by book as well as word: a word looked up in one book must never
// suppress the card another book's deck does not have yet.
TEST_F(FlashcardDeckTest, WindowDoesNotLeakAcrossBooks) {
  FlashcardDeck::clearEnrollCooldown();
  constexpr uint32_t kWindow = 5 * 60 * 1000;
  const std::string other = makeDevice();
  FlashcardDeck::enroll(cachePath, "alpha", "book one", "", 0, /*nowMs=*/1000, kWindow);
  FlashcardDeck::enroll(other, "alpha", "book two", "", 0, /*nowMs=*/2000, kWindow);

  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(other, "alpha", e));  // the second book got its own card
  EXPECT_EQ(e.excerpt, "book two");
}

// Deleting a card drops the window with it, or an immediate re-lookup would be throttled
// against a card that no longer exists and the word would silently not come back.
TEST_F(FlashcardDeckTest, DeleteClearsTheWindow) {
  FlashcardDeck::clearEnrollCooldown();
  constexpr uint32_t kWindow = 5 * 60 * 1000;
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx", "", 0, /*nowMs=*/1000, kWindow);
  ASSERT_TRUE(FlashcardDeck::remove(cachePath, "alpha"));
  EXPECT_EQ(FlashcardDeck::count(cachePath), 0);

  FlashcardDeck::enroll(cachePath, "alpha", "back again", "", 0, /*nowMs=*/2000, kWindow);
  EXPECT_EQ(FlashcardDeck::count(cachePath), 1);
  EXPECT_EQ(at(0).excerpt, "back again");
}

TEST_F(FlashcardDeckTest, LookupCountSurvivesGradeAndSuspend) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  FlashcardDeck::enroll(cachePath, "alpha", "ctx2");  // count -> 2
  FlashcardDeck::grade(cachePath, "alpha", /*correct=*/true, /*today=*/100);
  EXPECT_EQ(at(0).count, 2u);  // schedule change preserves count
  FlashcardDeck::suspend(cachePath, "alpha");
  EXPECT_EQ(at(0).count, 2u);
}

TEST_F(FlashcardDeckTest, LookupCountIsLocalNotSynced) {
  // A re-enrolled (count=3) card uploaded and merged onto a fresh peer arrives at
  // count 1: the lookup tally is per-device, never on the wire.
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1");
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1");
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1");
  EXPECT_EQ(at(0).count, 3u);

  uint8_t blob[2048];
  FlashcardDeck::BlobStats st;
  const size_t n = FlashcardDeck::serializeForUpload(cachePath, blob, sizeof(blob), &st);
  ASSERT_GT(n, 0u);

  const std::string peer = makeDevice();
  FlashcardDeck::mergeBlob(peer, blob, n, nullptr);
  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(peer, "alpha", e));
  EXPECT_EQ(e.count, 1u);  // peer starts its own tally
}

// Excerpt pipes used to round-trip verbatim: the excerpt is the line remainder, so a '|' in it
// could not break the fields before it. That stopped being true once the line grew optional
// trailing fields sniffed by "is it all-digits" — a legacy excerpt like "1999|was a good year"
// reads its leading token as the next optional field. Stripping at the write end is what makes
// every line this build produces unambiguous, so dictHash (and anything after it) is safe to add.
// The excerpt keeps its pipes: it is the line remainder, and writeCard always emits all three
// optional fields, so every delimiter parseLine tracks is consumed by the header.
TEST_F(FlashcardDeckTest, ExcerptWithEmbeddedPipesRoundTrips) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx|with|pipes|inside");
  EXPECT_EQ(at(0).excerpt, "ctx|with|pipes|inside");
}

// The field-sniffing hazard, pinned from the write side: an excerpt beginning with a numeric
// pipe-delimited token still round-trips intact, because dictHash occupies the slot that token
// would otherwise be mistaken for. This is what makes stripping unnecessary.
TEST_F(FlashcardDeckTest, ExcerptStartingWithNumericTokenSurvives) {
  FlashcardDeck::enroll(cachePath, "alpha", "1999|was a good year");
  EXPECT_EQ(at(0).excerpt, "1999|was a good year");
}

TEST_F(FlashcardDeckTest, ExcerptNewlinesSanitizedAndCapped) {
  FlashcardDeck::enroll(cachePath, "alpha", "line1\nline2\ttabbed");
  EXPECT_EQ(at(0).excerpt, "line1 line2 tabbed");

  std::string huge(300, 'x');
  FlashcardDeck::enroll(cachePath, "beta", huge);
  EXPECT_EQ(static_cast<int>(at(0).excerpt.size()), FlashcardDeck::EXCERPT_MAX);
}

TEST_F(FlashcardDeckTest, EmptyWordRejected) {
  EXPECT_FALSE(FlashcardDeck::enroll(cachePath, "", "ctx"));
  EXPECT_EQ(FlashcardDeck::count(cachePath), 0);
}

// --------------------------------------------------------------------------
// chapter field
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, ChapterRoundTrips) {
  FlashcardDeck::enroll(cachePath, "alpha", "the alpha wolf", "Chapter 3: Wolves");
  FlashcardDeck::Entry e = at(0);
  EXPECT_EQ(e.chapter, "Chapter 3: Wolves");
  EXPECT_EQ(e.excerpt, "the alpha wolf");  // excerpt (remainder) still intact
}

TEST_F(FlashcardDeckTest, ChapterPipeStripped) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx|with|pipes", "Part|II");
  EXPECT_EQ(at(0).chapter, "Part II");         // '|' collapsed so it can't break parse
  EXPECT_EQ(at(0).excerpt, "ctx|with|pipes");  // excerpt is the remainder — pipes are harmless
}

TEST_F(FlashcardDeckTest, ChapterPreservedOnEmptyReEnroll) {
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx", "Chapter 1");
  FlashcardDeck::enroll(cachePath, "alpha", "", "");  // re-lookup from history list
  EXPECT_EQ(at(0).chapter, "Chapter 1");
  EXPECT_EQ(at(0).excerpt, "first ctx");
}

TEST_F(FlashcardDeckTest, GradePreservesChapter) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Chapter 5");
  EXPECT_TRUE(FlashcardDeck::grade(cachePath, "alpha", true, 100));
  EXPECT_EQ(at(0).chapter, "Chapter 5");
  EXPECT_EQ(at(0).box, FlashcardDeck::TOP_BOX);
}

TEST_F(FlashcardDeckTest, LegacyFourFieldLineUpgradesCleanly) {
  // A pre-chapter deck line: word|box|dueDay|excerpt (three pipes).
  FILE* f = std::fopen((cachePath + "/dictionary_flashcards.txt").c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fputs("alpha|2|55|an old sentence\n", f);
  std::fclose(f);

  FlashcardDeck::Entry e = at(0);
  EXPECT_EQ(e.word, "alpha");
  EXPECT_EQ(e.box, 2);
  EXPECT_EQ(e.dueDay, 55u);
  EXPECT_EQ(e.chapter, "");                 // no chapter in legacy line
  EXPECT_EQ(e.excerpt, "an old sentence");  // excerpt preserved as the remainder
}

// --------------------------------------------------------------------------
// loadWindow / removeAt
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, LoadWindowNewestFirst) {
  for (const char* w : {"aa", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::Entry win[3];
  EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, 0, 3, win), 3);
  EXPECT_EQ(win[0].word, "d");  // newest
  EXPECT_EQ(win[1].word, "c");
  EXPECT_EQ(win[2].word, "b");

  EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, 3, 3, win), 1);  // only "aa" left
  EXPECT_EQ(win[0].word, "aa");
  EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, 4, 3, win), 0);  // past end
}

TEST_F(FlashcardDeckTest, LoadWindowWordsOnlySkipsExcerptAndChapter) {
  FlashcardDeck::enroll(cachePath, "alpha", "an alpha sentence", "Chapter 1");
  FlashcardDeck::enroll(cachePath, "beta", "a beta sentence", "Chapter 2");

  // Full load populates every field.
  FlashcardDeck::Entry full[2];
  EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, 0, 2, full, /*wordsOnly=*/false), 2);
  EXPECT_EQ(full[0].word, "beta");
  EXPECT_EQ(full[0].excerpt, "a beta sentence");
  EXPECT_EQ(full[0].chapter, "Chapter 2");

  // Word-only load fills word/box/dueDay but leaves excerpt/chapter empty (the
  // list-view fast path that avoids two string allocations per row).
  FlashcardDeck::Entry words[2];
  EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, 0, 2, words, /*wordsOnly=*/true), 2);
  EXPECT_EQ(words[0].word, "beta");
  EXPECT_EQ(words[1].word, "alpha");
  EXPECT_TRUE(words[0].excerpt.empty());
  EXPECT_TRUE(words[0].chapter.empty());
  EXPECT_TRUE(words[1].excerpt.empty());
  EXPECT_TRUE(words[1].chapter.empty());
}

TEST_F(FlashcardDeckTest, RemoveAtByFileIndex) {
  for (const char* w : {"aa", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  EXPECT_TRUE(FlashcardDeck::removeAt(cachePath, 1));  // oldest=0 -> removes "b"
  EXPECT_EQ(FlashcardDeck::count(cachePath), 2);
  EXPECT_EQ(at(0).word, "c");
  EXPECT_EQ(at(1).word, "aa");
  EXPECT_FALSE(FlashcardDeck::removeAt(cachePath, 5));  // out of range
}

TEST_F(FlashcardDeckTest, RemoveByWordDropsMatchingRowPreservesOrder) {
  for (const char* w : {"aa", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  EXPECT_TRUE(FlashcardDeck::remove(cachePath, "b"));
  EXPECT_EQ(FlashcardDeck::count(cachePath), 2);
  EXPECT_EQ(at(0).word, "c");  // newest-first order otherwise unchanged
  EXPECT_EQ(at(1).word, "aa");
}

TEST_F(FlashcardDeckTest, RemoveAbsentWordIsNoOp) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  EXPECT_FALSE(FlashcardDeck::remove(cachePath, "ghost"));
  EXPECT_EQ(FlashcardDeck::count(cachePath), 1);
  EXPECT_EQ(at(0).word, "alpha");
}

// --------------------------------------------------------------------------
// grade (I/O)
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, GradeMutatesRowPreservesExcerptAndOrder) {
  FlashcardDeck::enroll(cachePath, "alpha", "alpha ctx");
  FlashcardDeck::enroll(cachePath, "beta", "beta ctx");
  EXPECT_TRUE(FlashcardDeck::grade(cachePath, "alpha", /*correct=*/true, /*today=*/100));

  // Order unchanged: alpha still oldest, beta still newest. A first-showing hit skips a
  // rung, so alpha lands on the top box rather than box 1.
  EXPECT_EQ(at(1).word, "alpha");
  EXPECT_EQ(at(1).box, FlashcardDeck::TOP_BOX);
  EXPECT_EQ(at(1).dueDay, 100u + FlashcardDeck::BOX_INTERVAL_DAYS[FlashcardDeck::TOP_BOX]);
  EXPECT_EQ(at(1).excerpt, "alpha ctx");  // excerpt preserved
  EXPECT_EQ(at(0).word, "beta");
  EXPECT_EQ(at(0).box, 0);  // untouched
}

// Grading with no clock (today == 0) must not touch the deck. buildSession drops the due
// filter at today == 0, so a recorded grade would let the same card be re-drawn and promoted
// every session until it graduated without ever being spaced.
TEST_F(FlashcardDeckTest, GradeWithoutClockIsNoOp) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  FlashcardDeck::grade(cachePath, "alpha", true, 10);  // first showing -> top box, due 10 + 7
  ASSERT_EQ(at(0).box, FlashcardDeck::TOP_BOX);
  ASSERT_EQ(at(0).dueDay, 17u);

  EXPECT_FALSE(FlashcardDeck::grade(cachePath, "alpha", true, 0));
  EXPECT_EQ(at(0).box, FlashcardDeck::TOP_BOX);  // unchanged -- no promotion
  EXPECT_EQ(at(0).dueDay, 17u);                  // unchanged -- no epoch-relative date written
}

TEST_F(FlashcardDeckTest, GradeWithoutClockCannotGraduateACard) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  // Six correct grades is exactly what graduates a card when a clock is present.
  for (int i = 0; i < 6; i++) EXPECT_FALSE(FlashcardDeck::grade(cachePath, "alpha", true, 0));
  EXPECT_EQ(at(0).box, 0);
  EXPECT_FALSE(FlashcardDeck::isMastered(at(0).box));
}

TEST_F(FlashcardDeckTest, GradeAbsentWordIsNoOp) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  EXPECT_FALSE(FlashcardDeck::grade(cachePath, "ghost", true, 100));
  EXPECT_EQ(at(0).box, 0);
}

// --------------------------------------------------------------------------
// buildSession
// --------------------------------------------------------------------------

// Membership helper: the due-first scope interleaves four groups, so which cards were
// selected is the contract -- the caller shuffles, so their order in `out` is not.
static bool selected(const uint16_t* out, int n, uint16_t idx) {
  for (int i = 0; i < n; i++) {
    if (out[i] == idx) return true;
  }
  return false;
}

TEST_F(FlashcardDeckTest, BuildSessionAllNewSelectsEveryCard) {
  for (const char* w : {"aa", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  uint16_t out[8];
  // All cards are new (dueDay==0), so both due groups are empty and the two new-card
  // groups fill the window between them. newest-first indices: d=0, c=1, b=2, a=3.
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, /*today=*/10, 8, out);
  EXPECT_EQ(n, 4);
  for (uint16_t i = 0; i < 4; i++) EXPECT_TRUE(selected(out, n, i));
}

// The regression this design exists to prevent: due cards used to fill the whole window
// first, so on a deck with enough due cards a never-studied card could never appear.
TEST_F(FlashcardDeckTest, BuildSessionGivesDueAndNewCardsBothASlot) {
  for (const char* w : {"aa", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  // Schedule b and c as due (graded at day 5 -> due day 12). a and d stay new.
  FlashcardDeck::grade(cachePath, "b", true, 5);
  FlashcardDeck::grade(cachePath, "c", true, 5);

  // newest-first indices: d=0, c=1, b=2, a=3.
  uint16_t out[8];
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, /*today=*/12, 8, out);
  EXPECT_EQ(n, 4);
  EXPECT_TRUE(selected(out, n, 1));  // c, due
  EXPECT_TRUE(selected(out, n, 2));  // b, due
  EXPECT_TRUE(selected(out, n, 0));  // d, new
  EXPECT_TRUE(selected(out, n, 3));  // a, new
}

// A window too small for one card per group is spent on the group starvation actually
// harms: the card whose due day passed longest ago.
TEST_F(FlashcardDeckTest, BuildSessionRespectsCap) {
  for (const char* w : {"aa", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::grade(cachePath, "b", true, 5);  // due 12
  FlashcardDeck::grade(cachePath, "c", true, 9);  // due 16
  uint16_t out[1];
  EXPECT_EQ(FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, 16, 1, out), 1);
  EXPECT_EQ(out[0], 2);  // b -- due 12, overdue longer than c
}

// The most-neglected card gets in even when newer lookups outnumber it, and a recently
// looked-up card still gets its own slot rather than the pool going all-overdue.
TEST_F(FlashcardDeckTest, BuildSessionPairsTheMostOverdueWithTheMostRecent) {
  for (const char* w : {"aa", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::grade(cachePath, "aa", true, 1);  // due 8   (most overdue)
  FlashcardDeck::grade(cachePath, "b", true, 5);   // due 12
  FlashcardDeck::grade(cachePath, "c", true, 9);   // due 16
  FlashcardDeck::grade(cachePath, "d", true, 12);  // due 19  (most recently looked up)

  uint16_t out[2];
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, /*today=*/20, 2, out);
  EXPECT_EQ(n, 2);
  EXPECT_TRUE(selected(out, n, 3));  // aa, longest overdue
  EXPECT_TRUE(selected(out, n, 0));  // d, most recent
}

// The groups overlap whenever a tier holds fewer cards than two group budgets -- the same
// row is then both "oldest new" and "newest new". It must occupy one slot, not two.
TEST_F(FlashcardDeckTest, BuildSessionNeverSelectsACardTwice) {
  for (const char* w : {"aa", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  uint16_t out[8];
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, /*today=*/10, 8, out);
  ASSERT_EQ(n, 3);
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) EXPECT_NE(out[i], out[j]);
  }
}

// An empty group hands its share to the others: with no due cards at all, the two
// new-card groups must still fill the whole window, not half of it.
TEST_F(FlashcardDeckTest, BuildSessionEmptyGroupsGiveTheirSlotsAway) {
  char w[8];
  for (int i = 0; i < 12; i++) {
    snprintf(w, sizeof(w), "w%d", i);
    FlashcardDeck::enroll(cachePath, w, "");
  }
  uint16_t out[8];
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, /*today=*/10, 8, out);
  EXPECT_EQ(n, 8);  // full window, not the 4 one group could supply alone
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) EXPECT_NE(out[i], out[j]);
  }
}

TEST_F(FlashcardDeckTest, BuildSessionExcludesRetired) {
  for (const char* w : {"aa", "b"}) FlashcardDeck::enroll(cachePath, w, "");
  // Graduate "aa": 6 consecutive correct.
  for (int i = 0; i < 6; i++) FlashcardDeck::grade(cachePath, "aa", true, 1);
  EXPECT_TRUE(FlashcardDeck::isMastered(at(1).box));

  uint16_t out[8];
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::AllShuffled, 0, 8, out);
  EXPECT_EQ(n, 1);       // only "b" remains schedulable
  EXPECT_EQ(out[0], 0);  // b is newest
}

TEST_F(FlashcardDeckTest, BuildSessionClockUnavailableFallsBackToAll) {
  for (const char* w : {"aa", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::grade(cachePath, "b", true, 5);  // schedules b
  uint16_t out[8];
  // today==0 (clock unavailable): DueFirst falls back to non-retired newest-first.
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, 0, 8, out);
  EXPECT_EQ(n, 3);
  EXPECT_EQ(out[0], 0);  // c
  EXPECT_EQ(out[1], 1);  // b
  EXPECT_EQ(out[2], 2);  // a
}

// --------------------------------------------------------------------------
// computeStats
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, ComputeStatsTalliesBoxesDueMastered) {
  for (const char* w : {"aa", "b", "c", "d", "e", "f"}) FlashcardDeck::enroll(cachePath, w, "");
  // First hit fast-tracks aa to the top box, second graduates it.
  FlashcardDeck::grade(cachePath, "aa", true, 1);
  FlashcardDeck::grade(cachePath, "aa", true, 1);
  FlashcardDeck::grade(cachePath, "b", true, 5);   // top box, due 12
  FlashcardDeck::grade(cachePath, "d", true, 5);   // top box, due 12
  FlashcardDeck::grade(cachePath, "f", true, 10);  // top box, due 17 (future)
  // c, e remain new (box0, due0).

  const FlashcardDeck::Stats s = FlashcardDeck::computeStats(cachePath, /*today=*/12);
  EXPECT_EQ(s.total, 6);
  EXPECT_EQ(s.mastered, 1);
  EXPECT_EQ(s.boxHist[0], 2);                       // c, e new
  EXPECT_EQ(s.boxHist[FlashcardDeck::TOP_BOX], 3);  // b, d, f
  EXPECT_EQ(s.due, 4);                              // c, e (new) + b, d (due 12 <= 12)
  EXPECT_EQ(s.nextDueDay, 17u);                     // f is the only future-scheduled card
}

TEST_F(FlashcardDeckTest, ComputeStatsEmptyDeck) {
  const FlashcardDeck::Stats s = FlashcardDeck::computeStats(cachePath, 10);
  EXPECT_EQ(s.total, 0);
  EXPECT_EQ(s.due, 0);
  EXPECT_EQ(s.mastered, 0);
  EXPECT_EQ(s.nextDueDay, 0u);
}

TEST_F(FlashcardDeckTest, BuildSessionEmptyDeck) {
  uint16_t out[8];
  EXPECT_EQ(FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, 10, 8, out), 0);
}

// --------------------------------------------------------------------------
// hasDueCards -- the reader's inline-review gate. Must agree with computeStats().due > 0
// while stopping at the first hit instead of reading the whole deck.
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, HasDueCardsMissingFile) { EXPECT_FALSE(FlashcardDeck::hasDueCards(cachePath, 10)); }

TEST_F(FlashcardDeckTest, HasDueCardsTrueForNewCard) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");  // new: dueDay 0 counts as due
  EXPECT_TRUE(FlashcardDeck::hasDueCards(cachePath, 10));
}

TEST_F(FlashcardDeckTest, HasDueCardsFalseWhenAllScheduledAhead) {
  for (const char* w : {"aa", "b"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::grade(cachePath, "aa", true, 10);  // first showing -> top box, due 17
  FlashcardDeck::grade(cachePath, "b", true, 10);   // first showing -> top box, due 17
  EXPECT_FALSE(FlashcardDeck::hasDueCards(cachePath, 16));
  EXPECT_TRUE(FlashcardDeck::hasDueCards(cachePath, 17));  // due day arrives
}

TEST_F(FlashcardDeckTest, HasDueCardsFalseWhenAllRetiredOrSuspended) {
  for (const char* w : {"aa", "b"}) FlashcardDeck::enroll(cachePath, w, "");
  for (int i = 0; i < 6; i++) FlashcardDeck::grade(cachePath, "aa", true, 1);  // graduate -> RETIRED
  FlashcardDeck::suspend(cachePath, "b");
  EXPECT_FALSE(FlashcardDeck::hasDueCards(cachePath, 10));
}

TEST_F(FlashcardDeckTest, HasDueCardsAgreesWithComputeStats) {
  for (const char* w : {"aa", "b", "c", "d", "e", "f"}) FlashcardDeck::enroll(cachePath, w, "");
  for (int i = 0; i < 6; i++) FlashcardDeck::grade(cachePath, "aa", true, 1);
  FlashcardDeck::grade(cachePath, "b", true, 5);
  FlashcardDeck::grade(cachePath, "c", true, 5);
  FlashcardDeck::grade(cachePath, "d", true, 5);
  FlashcardDeck::grade(cachePath, "e", true, 5);
  FlashcardDeck::grade(cachePath, "f", true, 5);
  // Every non-retired card is now scheduled to day 7.
  for (uint32_t today : {6u, 7u, 8u}) {
    const bool expected = FlashcardDeck::computeStats(cachePath, today).due > 0;
    EXPECT_EQ(FlashcardDeck::hasDueCards(cachePath, today), expected) << "today=" << today;
  }
}

// --------------------------------------------------------------------------
// suspend / unsuspend
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, SuspendSetsSentinelAndPreservesContext) {
  FlashcardDeck::enroll(cachePath, "alpha", "the alpha wolf", "Chapter 3");
  FlashcardDeck::grade(cachePath, "alpha", true, 5);  // box1, dueDay set
  EXPECT_TRUE(FlashcardDeck::suspend(cachePath, "alpha"));

  const FlashcardDeck::Entry e = at(0);
  EXPECT_EQ(e.box, FlashcardDeck::SUSPENDED);
  EXPECT_TRUE(FlashcardDeck::isSuspended(e.box));
  EXPECT_EQ(e.dueDay, 0u);
  EXPECT_EQ(e.excerpt, "the alpha wolf");  // context preserved
  EXPECT_EQ(e.chapter, "Chapter 3");
}

TEST_F(FlashcardDeckTest, SuspendPreservesOrder) {
  for (const char* w : {"aa", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  EXPECT_TRUE(FlashcardDeck::suspend(cachePath, "b"));  // middle card
  EXPECT_EQ(at(0).word, "c");
  EXPECT_EQ(at(1).word, "b");  // still in place, just suspended
  EXPECT_EQ(at(1).box, FlashcardDeck::SUSPENDED);
  EXPECT_EQ(at(2).word, "aa");
}

TEST_F(FlashcardDeckTest, SuspendAbsentWordIsNoOp) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  EXPECT_FALSE(FlashcardDeck::suspend(cachePath, "ghost"));
  EXPECT_EQ(at(0).box, 0);
}

TEST_F(FlashcardDeckTest, UnsuspendRestoresAsNewCard) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1");
  FlashcardDeck::grade(cachePath, "alpha", true, 5);  // promoted off the new pool
  FlashcardDeck::suspend(cachePath, "alpha");
  EXPECT_TRUE(FlashcardDeck::unsuspend(cachePath, "alpha"));

  const FlashcardDeck::Entry e = at(0);
  EXPECT_EQ(e.box, 0u);  // back in the new pool
  EXPECT_EQ(e.dueDay, 0u);
  EXPECT_EQ(e.excerpt, "ctx");  // context still preserved
  EXPECT_EQ(e.chapter, "Ch1");
}

TEST_F(FlashcardDeckTest, SuspendedCardIsNotDue) {
  EXPECT_FALSE(FlashcardDeck::isDue(FlashcardDeck::SUSPENDED, 0, 100));
  EXPECT_FALSE(FlashcardDeck::isDue(FlashcardDeck::SUSPENDED, 50, 100));
}

TEST_F(FlashcardDeckTest, ComputeStatsCountsSuspendedSeparately) {
  for (const char* w : {"aa", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::suspend(cachePath, "b");

  const FlashcardDeck::Stats s = FlashcardDeck::computeStats(cachePath, /*today=*/10);
  EXPECT_EQ(s.total, 3);
  EXPECT_EQ(s.suspended, 1);
  EXPECT_EQ(s.mastered, 0);
  EXPECT_EQ(s.boxHist[0], 2);  // a, c -- suspended b not in the histogram
  EXPECT_EQ(s.due, 2);         // only a, c are due
}

TEST_F(FlashcardDeckTest, BuildSessionExcludesSuspended) {
  for (const char* w : {"aa", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::suspend(cachePath, "b");
  uint16_t out[8];
  // newest-first: c=0, b=1, a=2. Suspended b is dropped from normal scopes.
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::AllShuffled, 0, 8, out);
  EXPECT_EQ(n, 2);
  EXPECT_EQ(out[0], 0);  // c
  EXPECT_EQ(out[1], 2);  // a
}

TEST_F(FlashcardDeckTest, BuildSessionSuspendedScopeSelectsOnlySuspended) {
  for (const char* w : {"aa", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::suspend(cachePath, "b");
  FlashcardDeck::suspend(cachePath, "d");  // newest-first: d=0, c=1, b=2, a=3
  uint16_t out[8];
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::Suspended, /*today=*/10, 8, out);
  EXPECT_EQ(n, 2);
  EXPECT_EQ(out[0], 0);  // d (newest suspended)
  EXPECT_EQ(out[1], 2);  // b
}

// --------------------------------------------------------------------------
// Cross-device sync ("fc" blob)
// --------------------------------------------------------------------------

// enroll bumps the per-word version; grade/suspend preserve it (schedule-local).
TEST_F(FlashcardDeckTest, EnrollBumpsVersionScheduleChangesPreserveIt) {
  FlashcardDeck::enroll(cachePath, "cat", "the cat sat", "Ch1");
  const uint32_t catV = at(0).version;
  EXPECT_GT(catV, 0u);

  FlashcardDeck::enroll(cachePath, "dog", "a dog ran", "Ch1");
  EXPECT_GT(at(0).version, catV);  // newest enroll bumped past cat

  // Grading "cat" is a local schedule change -- version unchanged.
  FlashcardDeck::grade(cachePath, "cat", /*correct=*/true, /*today=*/10);
  FlashcardDeck::Entry cat;
  ASSERT_TRUE(findCard(cachePath, "cat", cat));
  EXPECT_EQ(cat.version, catV);
  EXPECT_EQ(cat.box, FlashcardDeck::TOP_BOX);

  // Suspend likewise preserves the version.
  FlashcardDeck::suspend(cachePath, "cat");
  ASSERT_TRUE(findCard(cachePath, "cat", cat));
  EXPECT_EQ(cat.version, catV);
}

// A enrolled card appears on B as a fresh box-0 card with its content + version.
TEST_F(FlashcardDeckTest, RoundTripCardContent) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "the cat sat", "Ch1");

  EXPECT_EQ(syncRound(cachePath, B), 1);  // one card added on B

  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(B, "cat", e));
  EXPECT_EQ(e.chapter, "Ch1");
  EXPECT_EQ(e.excerpt, "the cat sat");
  EXPECT_EQ(e.box, 0);  // received card is always new
  EXPECT_EQ(e.dueDay, 0u);
  EXPECT_EQ(e.version, at(0).version);  // same Lamport version as the source
}

// A newer enroll (higher version) updates B's copy in place.
TEST_F(FlashcardDeckTest, VersionWinsNewerEnroll) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "old sentence", "Ch1");
  syncRound(cachePath, B);

  FlashcardDeck::enroll(cachePath, "cat", "new sentence", "Ch2");  // re-enroll -> higher version
  syncRound(cachePath, B);

  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(B, "cat", e));
  EXPECT_EQ(e.excerpt, "new sentence");
  EXPECT_EQ(e.chapter, "Ch2");
  EXPECT_EQ(e.version, at(0).version);
  EXPECT_EQ(FlashcardDeck::count(B), 1);  // updated in place, not duplicated
}

// A delete on A propagates to B and removes its card.
TEST_F(FlashcardDeckTest, TombstonePropagation) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "x", "Ch1");
  FlashcardDeck::enroll(cachePath, "dog", "y", "Ch1");
  syncRound(cachePath, B);
  EXPECT_EQ(FlashcardDeck::count(B), 2);

  FlashcardDeck::remove(cachePath, "cat");  // writes a tombstone
  int deleted = 0;
  syncRound(cachePath, B, 4096, 1, &deleted);

  EXPECT_EQ(deleted, 1);
  FlashcardDeck::Entry e;
  EXPECT_FALSE(findCard(B, "cat", e));
  EXPECT_TRUE(findCard(B, "dog", e));
}

// Re-enrolling a deleted word (higher version) beats the tombstone on merge.
TEST_F(FlashcardDeckTest, ReEnrollAfterDeleteWins) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "first", "Ch1");
  syncRound(cachePath, B);
  FlashcardDeck::remove(cachePath, "cat");
  syncRound(cachePath, B);
  FlashcardDeck::Entry e;
  ASSERT_FALSE(findCard(B, "cat", e));

  FlashcardDeck::enroll(cachePath, "cat", "reborn", "Ch3");  // version > tombstone
  syncRound(cachePath, B);

  ASSERT_TRUE(findCard(B, "cat", e));
  EXPECT_EQ(e.excerpt, "reborn");
}

// A remote content update must NOT reset B's local Leitner schedule.
TEST_F(FlashcardDeckTest, FieldLevelMergePreservesSchedule) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "ctx", "Ch1");
  syncRound(cachePath, B);

  // B grades "cat" onto the top box (its own schedule).
  FlashcardDeck::grade(B, "cat", /*correct=*/true, /*today=*/10);
  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(B, "cat", e));
  ASSERT_EQ(e.box, FlashcardDeck::TOP_BOX);
  const uint32_t dueBefore = e.dueDay;

  // A re-enrolls "cat" with new content (higher version) and syncs to B.
  FlashcardDeck::enroll(cachePath, "cat", "updated ctx", "Ch9");
  syncRound(cachePath, B);

  ASSERT_TRUE(findCard(B, "cat", e));
  EXPECT_EQ(e.box, FlashcardDeck::TOP_BOX);  // schedule preserved
  EXPECT_EQ(e.dueDay, dueBefore);            // schedule preserved
  EXPECT_EQ(e.excerpt, "updated ctx");       // content updated
  EXPECT_EQ(e.chapter, "Ch9");
}

// A suspended card on B stays suspended through a remote content update.
TEST_F(FlashcardDeckTest, SuspendSurvivesRemoteUpdate) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "ctx", "Ch1");
  syncRound(cachePath, B);
  FlashcardDeck::suspend(B, "cat");

  FlashcardDeck::enroll(cachePath, "cat", "updated", "Ch2");
  syncRound(cachePath, B);

  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(B, "cat", e));
  EXPECT_EQ(e.box, FlashcardDeck::SUSPENDED);
  EXPECT_EQ(e.excerpt, "updated");
}

// A deck larger than one slice fully propagates over repeated bounded syncs,
// with no lost or duplicated cards (rolling cursor + delta coverage).
TEST_F(FlashcardDeckTest, RollingCursorCoversLargeDeck) {
  const std::string B = makeDevice();
  const int N = 50;
  const std::string longExcerpt(100, 'x');  // fat cards so a 512B slice holds only a few
  for (int i = 0; i < N; i++) {
    char w[16];
    std::snprintf(w, sizeof(w), "word%02d", i);
    FlashcardDeck::enroll(cachePath, w, longExcerpt, "Ch");
  }

  int rounds = 0;
  while (FlashcardDeck::count(B) < N && rounds < 400) {
    syncRound(cachePath, B, /*cap=*/512);
    rounds++;
  }
  EXPECT_LT(rounds, 400);                 // converged
  EXPECT_EQ(FlashcardDeck::count(B), N);  // every card arrived, none duplicated

  // Every source word is present exactly once on B.
  for (int i = 0; i < N; i++) {
    char w[16];
    std::snprintf(w, sizeof(w), "word%02d", i);
    FlashcardDeck::Entry e;
    EXPECT_TRUE(findCard(B, w, e)) << "missing " << w;
  }
  // Extra rounds are idempotent -- count stays N (rolling re-sends are no-ops).
  for (int k = 0; k < 5; k++) syncRound(cachePath, B, 512);
  EXPECT_EQ(FlashcardDeck::count(B), N);
}

// A new tombstone is pushed once as a delta delete, then only heals silently -- it is
// NOT re-counted as a "new" delete every sync. This lets the on-screen "new:-N" tomb
// count converge to 0 instead of perpetually re-broadcasting the whole tomb pile.
TEST_F(FlashcardDeckTest, NewTombPushedOnceThenHealsSilently) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "x", "Ch1");
  FlashcardDeck::enroll(cachePath, "dog", "y", "Ch1");
  syncRound(cachePath, B);  // both cards to B; A's lastVer advances past the enrolls

  FlashcardDeck::remove(cachePath, "cat");  // new tombstone, version > lastVer

  std::vector<uint8_t> buf(4096);
  FlashcardDeck::BlobStats st;
  FlashcardDeck::serializeForUpload(cachePath, buf.data(), buf.size(), &st);
  EXPECT_EQ(st.tombCount, 1);                     // counts as a NEW delete this once
  EXPECT_EQ(st.tombRollCount, 0);                 // not yet healing
  FlashcardDeck::commitUpload(cachePath, st, 1);  // lastVer catches up to the tomb

  // Next serialize: the tomb is now OLD (version <= lastVer). It must not re-count as
  // a new delete -- it only re-broadcasts via the rolling heal (hidden from "-N").
  st = FlashcardDeck::BlobStats{};
  FlashcardDeck::serializeForUpload(cachePath, buf.data(), buf.size(), &st);
  EXPECT_EQ(st.tombCount, 0);      // converged: no "new" delete
  EXPECT_GE(st.tombRollCount, 1);  // still healed in the background
}

// An OLD tombstone (already below the watermark) still reaches a brand-new peer via
// the rolling tomb heal, so a late-joining device can't resurrect a deleted word.
// Proven by the merge deleting the peer's own copy of the word.
TEST_F(FlashcardDeckTest, OldTombHealsToFreshPeer) {
  const std::string B = makeDevice();
  FlashcardDeck::enroll(cachePath, "cat", "x", "Ch1");
  FlashcardDeck::enroll(cachePath, "dog", "y", "Ch1");
  syncRound(cachePath, B);
  FlashcardDeck::remove(cachePath, "cat");
  // Sync several times so the cat-tomb falls below A's watermark (becomes "old").
  for (int i = 0; i < 3; i++) syncRound(cachePath, B);

  // Fresh device C with its own (lower-version) copy of "cat". A's old tomb must still
  // propagate and delete it.
  const std::string C = makeDevice();
  FlashcardDeck::enroll(C, "cat", "localcopy", "Ch");
  int totalDeleted = 0;
  for (int i = 0; i < 10; i++) {
    int d = 0;
    syncRound(cachePath, C, 4096, 1, &d);
    totalDeleted += d;
  }
  EXPECT_GE(totalDeleted, 1);  // the old cat-tomb reached C and removed its card
  FlashcardDeck::Entry e;
  EXPECT_FALSE(findCard(C, "cat", e));  // not resurrected
  EXPECT_TRUE(findCard(C, "dog", e));   // the live card also arrived
}

// A legacy 3-field watermark file (written before tombCursorIndex existed) loads
// cleanly: the first three fields parse and the new field defaults to 0. No format
// version bump -- the file is whitespace-delimited and forward/backward tolerant.
TEST_F(FlashcardDeckTest, LegacyThreeFieldWatermarkLoadsTombCursorZero) {
  const std::string path = cachePath + "/dictionary_flashcards.sync";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fputs("7 3 2", f);  // lastVer cursorIndex lastDeviceCount, no 4th field
  std::fclose(f);

  const FlashcardDeck::SyncWatermark wm = FlashcardDeck::loadWatermark(cachePath);
  EXPECT_EQ(wm.lastVer, 7u);
  EXPECT_EQ(wm.cursorIndex, 3u);
  EXPECT_EQ(wm.lastDeviceCount, 2u);
  EXPECT_EQ(wm.tombCursorIndex, 0u);  // defaulted, not garbage
}

// Adaptive slice cap: floor when constrained, grows with heap, clamped under the
// 64 KB GET aggregate regardless of inputs.
TEST_F(FlashcardDeckTest, AdaptiveSliceCapSizing) {
  // Low heap -> floor.
  EXPECT_EQ(FlashcardDeck::adaptiveSliceCap(/*freeHeap=*/40 * 1024, /*devices=*/1), FlashcardDeck::FC_SLICE_FLOOR);
  // High heap, few devices -> ceiling.
  EXPECT_EQ(FlashcardDeck::adaptiveSliceCap(/*freeHeap=*/100 * 1024, /*devices=*/2), FlashcardDeck::FC_SLICE_CEIL);
  // Never below the floor even with many devices.
  EXPECT_GE(FlashcardDeck::adaptiveSliceCap(80 * 1024, 8), FlashcardDeck::FC_SLICE_FLOOR);
  // Aggregate stays under the 64 KB GET cap for the worst case (8 devices, each
  // a maxed dh blob + counters + this slice).
  constexpr size_t kReserve = 4 * 1024 + 256;
  for (uint32_t devs : {1u, 2u, 4u, 8u}) {
    for (uint32_t heap : {40u, 56u, 72u, 120u}) {
      const size_t cap = FlashcardDeck::adaptiveSliceCap(heap * 1024, devs);
      EXPECT_LE(devs * (kReserve + cap), 64u * 1024u) << "devs=" << devs << " heap=" << heap;
    }
  }
}

// Legacy deck lines (no inline version) parse as version 0 with content intact.
TEST_F(FlashcardDeckTest, LegacyLinesParseAsVersionZero) {
  // Hand-write a legacy 5-field line (word|box|dueDay|chapter|excerpt) and a
  // legacy 4-field line (word|box|dueDay|excerpt) -- neither has a version field.
  const std::string path = cachePath + "/dictionary_flashcards.txt";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const char* lines = "old5|2|40|ChA|five field excerpt\nold4|1|0|four field excerpt\n";
  std::fwrite(lines, 1, std::strlen(lines), f);
  std::fclose(f);

  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(cachePath, "old5", e));
  EXPECT_EQ(e.box, 2);
  EXPECT_EQ(e.dueDay, 40u);
  EXPECT_EQ(e.chapter, "ChA");
  EXPECT_EQ(e.excerpt, "five field excerpt");
  EXPECT_EQ(e.version, 0u);

  ASSERT_TRUE(findCard(cachePath, "old4", e));
  EXPECT_EQ(e.box, 1);
  EXPECT_EQ(e.chapter, "");
  EXPECT_EQ(e.excerpt, "four field excerpt");
  EXPECT_EQ(e.version, 0u);
}

// --------------------------------------------------------------------------
// dictHash: which dictionary a card was saved from
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, DictHashRoundTrips) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 12345u);
  EXPECT_EQ(at(0).dictHash, 12345u);
}

TEST_F(FlashcardDeckTest, DictHashDefaultsToZeroWhenNotSupplied) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  EXPECT_EQ(at(0).dictHash, 0u);
}

// A re-lookup from the history list has no page and no dictionary context, and passes 0. That
// must not erase what the original in-book lookup recorded -- same rule excerpt/chapter follow.
TEST_F(FlashcardDeckTest, DictHashPreservedOnReEnrollWithoutContext) {
  FlashcardDeck::enroll(cachePath, "alpha", "first ctx", "Ch1", 999u);
  FlashcardDeck::enroll(cachePath, "alpha", "", "", 0u);
  EXPECT_EQ(at(0).dictHash, 999u);
}

// An existing association is the user's to change, via the definition screen's explicit "Set
// Dict" -- never by a later lookup landing in some other dictionary. Without this a set choice
// survives only until the next time the word is looked up.
TEST_F(FlashcardDeckTest, DictHashNotOverwrittenByALaterLookupElsewhere) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 111u);
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 222u);
  EXPECT_EQ(at(0).dictHash, 111u);
  // ...and the explicit set is what does change it.
  ASSERT_TRUE(FlashcardDeck::setCardDict(cachePath, "alpha", 222u));
  EXPECT_EQ(at(0).dictHash, 222u);
  // A lookup after the set leaves the set value alone too.
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 111u);
  EXPECT_EQ(at(0).dictHash, 222u);
}

// A card that records nothing yet is still stamped by the next lookup -- otherwise legacy
// cards and cards synced from a peer that sent no 'D' line could never acquire one.
TEST_F(FlashcardDeckTest, DictHashStampedOnACardThatRecordsNone) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 0u);
  ASSERT_EQ(at(0).dictHash, 0u);
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 333u);
  EXPECT_EQ(at(0).dictHash, 333u);
}

// The local schedule must survive a dictHash write, and the dictHash must survive a grade --
// they are stored on the same line but owned by different halves of the system.
TEST_F(FlashcardDeckTest, DictHashSurvivesGradeAndScheduleSurvivesDictHash) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 777u);
  FlashcardDeck::grade(cachePath, "alpha", /*correct=*/true, /*today=*/100);
  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(cachePath, "alpha", e));
  EXPECT_EQ(e.dictHash, 777u);
  EXPECT_EQ(e.box, FlashcardDeck::TOP_BOX);
}

// A card written before dictHash existed: 7 fields, no hash. Must parse with dictHash 0 and an
// intact excerpt rather than reading the excerpt's leading token as a hash.
TEST_F(FlashcardDeckTest, LegacySevenFieldLineParsesWithoutDictHash) {
  const std::string path = cachePath + "/dictionary_flashcards.txt";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const char* lines = "old7|2|40|ChA|9|3|seven field excerpt\n";
  std::fwrite(lines, 1, std::strlen(lines), f);
  std::fclose(f);

  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(cachePath, "old7", e));
  EXPECT_EQ(e.version, 9u);
  EXPECT_EQ(e.count, 3u);
  EXPECT_EQ(e.dictHash, 0u);
  EXPECT_EQ(e.excerpt, "seven field excerpt");
}

// cardDict(): the query the definition screen needs to tell "already this card's dictionary" from
// "this card records none" -- setCardDict collapses both into a bool and cannot be asked.
TEST_F(FlashcardDeckTest, CardDictReportsMissingCard) {
  FlashcardDeck::enroll(cachePath, "present", "ctx", "", 4242u);
  uint32_t hash = 0xDEADBEEF;
  EXPECT_FALSE(FlashcardDeck::cardDict(cachePath, "absent", hash));
  EXPECT_EQ(hash, 0xDEADBEEFu);  // untouched on the miss, so a caller can pre-seed it
}

TEST_F(FlashcardDeckTest, CardDictReturnsRecordedHash) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "", 4242u);
  FlashcardDeck::enroll(cachePath, "beta", "ctx", "", 777u);
  uint32_t hash = 0;
  ASSERT_TRUE(FlashcardDeck::cardDict(cachePath, "alpha", hash));
  EXPECT_EQ(hash, 4242u);
  ASSERT_TRUE(FlashcardDeck::cardDict(cachePath, "beta", hash));
  EXPECT_EQ(hash, 777u);
}

// A legacy 7-field line has no hash field at all. It must report FOUND with 0, not missing:
// the two mean different things to the caller (0 = offer to stamp it, missing = no card).
TEST_F(FlashcardDeckTest, CardDictReturnsZeroForLegacyLine) {
  const std::string path = cachePath + "/dictionary_flashcards.txt";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const char* lines = "old7|2|40|ChA|9|3|seven field excerpt\n";
  std::fwrite(lines, 1, std::strlen(lines), f);
  std::fclose(f);

  uint32_t hash = 0xFFFFFFFF;
  ASSERT_TRUE(FlashcardDeck::cardDict(cachePath, "old7", hash));
  EXPECT_EQ(hash, 0u);
}

// The documented limit of the all-digits sniffing, pinned so a future change has to notice it: a
// PRE-EXISTING line whose excerpt still contains a pipe after a numeric token is misread. Lines
// this build writes cannot hit it -- enroll() strips '|' from excerpts (see
// ExcerptStartingWithNumericTokenSurvives) -- but a SHORT legacy line can still trip it.
TEST_F(FlashcardDeckTest, LegacyExcerptWithNumericPipeTokenIsAbsorbedAsDictHash) {
  const std::string path = cachePath + "/dictionary_flashcards.txt";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const char* lines = "old|0|0|ChA|9|3|1999|was a good year\n";
  std::fwrite(lines, 1, std::strlen(lines), f);
  std::fclose(f);

  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(cachePath, "old", e));
  EXPECT_EQ(e.dictHash, 1999u);  // absorbed -- the known, bounded cost
  EXPECT_EQ(e.excerpt, "was a good year");
}

// --------------------------------------------------------------------------
// dictHash over the sync wire (the 'D' line)
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, DictHashPropagatesToPeer) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1", 4242u);
  uint8_t blob[2048];
  FlashcardDeck::BlobStats st;
  const size_t n = FlashcardDeck::serializeForUpload(cachePath, blob, sizeof(blob), &st);
  ASSERT_GT(n, 0u);

  const std::string peer = makeDevice();
  FlashcardDeck::mergeBlob(peer, blob, n, nullptr);
  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(peer, "alpha", e));
  EXPECT_EQ(e.dictHash, 4242u);
  EXPECT_EQ(e.excerpt, "ctx");  // the card line itself is untouched by the 'D' line
}

// A card with no association emits no 'D' line, and the peer simply has none.
TEST_F(FlashcardDeckTest, NoDictHashMeansNoAssociationOnPeer) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  uint8_t blob[2048];
  FlashcardDeck::BlobStats st;
  const size_t n = FlashcardDeck::serializeForUpload(cachePath, blob, sizeof(blob), &st);
  ASSERT_GT(n, 0u);

  const std::string peer = makeDevice();
  FlashcardDeck::mergeBlob(peer, blob, n, nullptr);
  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(peer, "alpha", e));
  EXPECT_EQ(e.dictHash, 0u);
}

// A 'D' for a word the peer does not have must be dropped, not create a card.
TEST_F(FlashcardDeckTest, DictLineForUnknownWordIsIgnored) {
  const std::string peer = makeDevice();
  const char* blob = "Dghost|55|7\n";
  const int added = FlashcardDeck::mergeBlob(peer, reinterpret_cast<const uint8_t*>(blob), std::strlen(blob), nullptr);
  EXPECT_EQ(added, 0);
  EXPECT_EQ(FlashcardDeck::count(peer), 0);
}

// The peer's own Leitner schedule is local and must not be disturbed by an incoming association.
TEST_F(FlashcardDeckTest, DictLineDoesNotDisturbLocalSchedule) {
  const std::string peer = makeDevice();
  FlashcardDeck::enroll(peer, "alpha", "ctx");
  FlashcardDeck::grade(peer, "alpha", /*correct=*/true, /*today=*/100);
  FlashcardDeck::Entry before;
  ASSERT_TRUE(findCard(peer, "alpha", before));

  const std::string line = "Dalpha|8080|" + std::to_string(before.version) + "\n";
  FlashcardDeck::mergeBlob(peer, reinterpret_cast<const uint8_t*>(line.data()), line.size(), nullptr);

  FlashcardDeck::Entry after;
  ASSERT_TRUE(findCard(peer, "alpha", after));
  EXPECT_EQ(after.dictHash, 8080u);
  EXPECT_EQ(after.box, before.box);
  EXPECT_EQ(after.dueDay, before.dueDay);
  EXPECT_EQ(after.excerpt, before.excerpt);
}

// A blob from an older device carries no 'D' lines at all; merging must still work unchanged.
TEST_F(FlashcardDeckTest, BlobWithoutDictLinesStillMerges) {
  const std::string peer = makeDevice();
  const char* blob = "Halpha|ChA|5|some excerpt\n";
  const int added = FlashcardDeck::mergeBlob(peer, reinterpret_cast<const uint8_t*>(blob), std::strlen(blob), nullptr);
  EXPECT_EQ(added, 1);
  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(peer, "alpha", e));
  EXPECT_EQ(e.excerpt, "some excerpt");
  EXPECT_EQ(e.dictHash, 0u);
}

// --- Stopword filter -----------------------------------------------------------
//
// Enrollment is automatic on every in-book lookup, and LookupHistory::addWordIf drops
// closed-class function words. Without the same filter here, tapping "a" was kept out of the
// history log and still created a card — the two per-book lists disagreeing about one lookup.

TEST_F(FlashcardDeckTest, StopwordsAreNotEnrolled) {
  for (const char* w : {"a", "the", "of", "would", "themselves"}) {
    EXPECT_FALSE(FlashcardDeck::enroll(cachePath, w, "some sentence")) << w;
  }
  EXPECT_EQ(FlashcardDeck::count(cachePath), 0);
}

TEST_F(FlashcardDeckTest, StopwordFilterIsCaseInsensitiveAndSparesContentWords) {
  EXPECT_FALSE(FlashcardDeck::enroll(cachePath, "The", "capitalised at a sentence start"));
  // Deliberately omitted from the list because their content sense is common (DictStopwords.h).
  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "will", "a last will and testament"));
  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "serendipity", "pure serendipity"));

  FlashcardDeck::Entry e;
  EXPECT_FALSE(findCard(cachePath, "the", e));
  EXPECT_TRUE(findCard(cachePath, "will", e));
  EXPECT_TRUE(findCard(cachePath, "serendipity", e));
}

// The filter guards automatic capture, not the sync merge: a peer on older firmware may still
// send a stopword card, and dropping it here would break Lamport convergence. Mirrors the same
// carve-out LookupHistory::mergeBlob makes.
TEST_F(FlashcardDeckTest, StopwordFilterDoesNotApplyToTheSyncMerge) {
  const std::string peer = makeDevice();
  const char* blob = "Hthe|ChA|5|sent by an older device\n";
  EXPECT_EQ(FlashcardDeck::mergeBlob(peer, reinterpret_cast<const uint8_t*>(blob), std::strlen(blob), nullptr), 1);
  FlashcardDeck::Entry e;
  EXPECT_TRUE(findCard(peer, "the", e));
}

// --- Page anchors ----------------------------------------------------------------------
//
// The chapter field doubles as the lookup's page anchor: enroll appends " X/Y" to the title,
// and the reader's page underline resolves a card to a page from it. The parse is shared with
// the card face's footer, so one rule covers both.

TEST_F(FlashcardDeckTest, ParseChapterPageSplitsTitleFromToken) {
  const char* ch = "Chapter 22: Sign of the Bident 11/27";
  int titleLen = -1, page = -1, count = -1;
  ASSERT_TRUE(FlashcardDeck::parseChapterPage(ch, static_cast<int>(std::strlen(ch)), &titleLen, &page, &count));
  EXPECT_EQ(std::string(ch, titleLen), "Chapter 22: Sign of the Bident");
  EXPECT_EQ(page, 11);
  EXPECT_EQ(count, 27);
}

TEST_F(FlashcardDeckTest, ParseChapterPageRejectsWhatIsNotAToken) {
  int titleLen = -1, page = -1, count = -1;
  // A title with no token at all, and one whose last word is digits but carries no '/'.
  const char* plain = "Chapter 22";
  EXPECT_FALSE(FlashcardDeck::parseChapterPage(plain, static_cast<int>(std::strlen(plain)), &titleLen, &page, &count));
  const char* noSlash = "Room 101";
  EXPECT_FALSE(
      FlashcardDeck::parseChapterPage(noSlash, static_cast<int>(std::strlen(noSlash)), &titleLen, &page, &count));
  // A token cut in half by the chapter cap is not a token either.
  const char* truncated = "A very long chapter title 11/";
  ASSERT_TRUE(
      FlashcardDeck::parseChapterPage(truncated, static_cast<int>(std::strlen(truncated)), &titleLen, &page, &count));
  EXPECT_EQ(count, 0);  // no denominator: the reader treats this as unanchored
  EXPECT_EQ(titleLen, static_cast<int>(std::strlen("A very long chapter title")));
  // Nothing at all.
  EXPECT_FALSE(FlashcardDeck::parseChapterPage(nullptr, 0, &titleLen, &page, &count));
}

TEST_F(FlashcardDeckTest, ParseChapterPageKeepsATitleThatEndsInDigits) {
  const char* ch = "Part 3 4/9";
  int titleLen = -1, page = -1, count = -1;
  ASSERT_TRUE(FlashcardDeck::parseChapterPage(ch, static_cast<int>(std::strlen(ch)), &titleLen, &page, &count));
  EXPECT_EQ(std::string(ch, titleLen), "Part 3");
  EXPECT_EQ(page, 4);
  EXPECT_EQ(count, 9);
}

struct Anchor {
  std::string word;
  std::string title;
  int page;
  int pageCount;
};

TEST_F(FlashcardDeckTest, ForEachCardAnchorStreamsWordsWithTheirPages) {
  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "pews", "lined with pews.", "Sign of the Bident 11/27"));
  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "helix", "a double helix", "Sign of the Bident 12/27"));
  // A card with no page token at all (a legacy card, or one synced from a peer).
  EXPECT_TRUE(FlashcardDeck::enroll(cachePath, "vermiform", "nasty vermiform creature", "Sign of the Bident"));

  std::vector<Anchor> got;
  ASSERT_TRUE(FlashcardDeck::forEachCardAnchor(
      cachePath,
      [](void* ctx, const char* word, int wordLen, const char* title, int titleLen, int page, int pageCount,
         const char* excerpt, int excerptLen) {
        (void)excerpt;
        (void)excerptLen;
        static_cast<std::vector<Anchor>*>(ctx)->push_back(
            Anchor{std::string(word, wordLen), std::string(title, titleLen > 0 ? titleLen : 0), page, pageCount});
        return true;
      },
      &got));

  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].word, "pews");
  EXPECT_EQ(got[0].title, "Sign of the Bident");
  EXPECT_EQ(got[0].page, 11);
  EXPECT_EQ(got[0].pageCount, 27);
  EXPECT_EQ(got[1].word, "helix");
  EXPECT_EQ(got[1].page, 12);
  // Unanchored card: the title survives whole, the page reports 0.
  EXPECT_EQ(got[2].word, "vermiform");
  EXPECT_EQ(got[2].title, "Sign of the Bident");
  EXPECT_EQ(got[2].page, 0);
  EXPECT_EQ(got[2].pageCount, 0);
}

// --- Surface form ----------------------------------------------------------------------
//
// A "Did you mean?" lookup files the card under the SUGGESTION, so the card word can differ
// from the word the page printed. Both marks (the excerpt underline and the reader-page one)
// have to land on the printed form, which is recovered from the card's own excerpt.

static std::string surfaceOf(const std::string& word, const std::string& excerpt) {
  int len = 0;
  const char* p = FlashcardDeck::findSurfaceForm(word.c_str(), static_cast<int>(word.size()), excerpt.c_str(),
                                                 static_cast<int>(excerpt.size()), &len);
  return p ? std::string(p, static_cast<size_t>(len)) : std::string();
}

TEST_F(FlashcardDeckTest, SurfaceFormPrefersAnExactOccurrence) {
  EXPECT_EQ(surfaceOf("helix", "in the form of a double helix that spiraled"), "helix");
  // Case as printed, not as filed.
  EXPECT_EQ(surfaceOf("citra", "Citra followed a stone path"), "Citra");
  // An exact hit wins even when an inflected sibling sits in the same sentence.
  EXPECT_EQ(surfaceOf("pontificate", "he would pontificate; his pontifications were tedious"), "pontificate");
}

TEST_F(FlashcardDeckTest, SurfaceFormRecoversTheInflectedPrintedWord) {
  EXPECT_EQ(surfaceOf("pontificate", "Goddard's pontifications were"), "pontifications");
  // Trailing punctuation belongs to the sentence, not the word.
  EXPECT_EQ(surfaceOf("pontificate", "and so, pontifications, endlessly"), "pontifications");
  // The headword inside a longer printed word is not an occurrence of it: the mark covers the
  // word the page shows, not its first half.
  EXPECT_EQ(surfaceOf("administer", "\"Gleaning is performed, not administered,\" Scythe Goddard"), "administered");
  EXPECT_EQ(surfaceOf("helix", "a nasty vermiform creature and some helices"), "helices");
}

TEST_F(FlashcardDeckTest, SurfaceFormRejectsWhatIsMerelySimilar) {
  // Four shared letters out of seven is a coincidence, not an inflection.
  EXPECT_EQ(surfaceOf("pontificate", "he fell from the pontoon"), "");
  // Short words cannot clear the minimum prefix — "ox" must not drag in "oxen".
  EXPECT_EQ(surfaceOf("ox", "the oxen were slow"), "");
  // Nothing in common at all.
  EXPECT_EQ(surfaceOf("pontificate", "a stone fracturing and shooting forth bolts"), "");
}

TEST_F(FlashcardDeckTest, SurfaceFormIsExactOnlyForCjk) {
  const std::string sentence = "\xE4\xBB\x96\xE6\x98\xAF\xE4\xB8\xAD\xE5\x9B\xBD\xE4\xBA\xBA";  // 他是中国人
  EXPECT_EQ(surfaceOf("\xE4\xB8\xAD\xE5\x9B\xBD\xE4\xBA\xBA", sentence), "\xE4\xB8\xAD\xE5\x9B\xBD\xE4\xBA\xBA");
  // A CJK headword absent from the excerpt gets no prefix-family fallback: 中国 must not be
  // recovered from a sentence that only contains 中文.
  const std::string other = "\xE4\xBB\x96\xE5\xAD\xA6\xE4\xB8\xAD\xE6\x96\x87";  // 他学中文
  EXPECT_EQ(surfaceOf("\xE4\xB8\xAD\xE5\x9B\xBD", other), "");
}

TEST_F(FlashcardDeckTest, SurfaceFormHandlesEmptyInput) {
  EXPECT_EQ(surfaceOf("pontificate", ""), "");
  EXPECT_EQ(surfaceOf("", "Goddard's pontifications were"), "");
}

}  // namespace
