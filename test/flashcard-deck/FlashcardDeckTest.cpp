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

TEST_F(FlashcardDeckTest, GradeTablePromotesAndSchedules) {
  uint8_t box = 0;
  uint32_t due = 0;
  const uint32_t today = 100;

  // Six consecutive correct recalls graduate (box 0 -> 5 -> RETIRED).
  const uint16_t expectedInterval[5] = {2, 4, 8, 16, 16};  // intervals on promotion into box 1..5
  for (int i = 0; i < 5; i++) {
    FlashcardDeck::applyGrade(box, due, /*correct=*/true, today);
    EXPECT_EQ(box, i + 1);
    EXPECT_EQ(due, today + expectedInterval[i]);
  }
  // 6th correct at the top box graduates; dueDay left untouched.
  const uint32_t dueBefore = due;
  FlashcardDeck::applyGrade(box, due, true, today);
  EXPECT_EQ(box, FlashcardDeck::RETIRED);
  EXPECT_EQ(due, dueBefore);
  EXPECT_TRUE(FlashcardDeck::isMastered(box));
}

TEST_F(FlashcardDeckTest, GradeMissResetsToBoxZeroWithRelearnInterval) {
  uint8_t box = 4;
  uint32_t due = 200;
  const uint32_t today = 100;
  FlashcardDeck::applyGrade(box, due, /*correct=*/false, today);
  EXPECT_EQ(box, 0);
  EXPECT_EQ(due, today + FlashcardDeck::BOX_INTERVAL_DAYS[0]);  // relearn = +1
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

TEST_F(FlashcardDeckTest, ExcerptWithEmbeddedPipesRoundTrips) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx|with|pipes|inside");
  EXPECT_EQ(at(0).excerpt, "ctx|with|pipes|inside");
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
  EXPECT_EQ(at(0).excerpt, "ctx|with|pipes");  // excerpt unaffected
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
  EXPECT_EQ(at(0).box, 1);
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
  for (const char* w : {"a", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::Entry win[3];
  EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, 0, 3, win), 3);
  EXPECT_EQ(win[0].word, "d");  // newest
  EXPECT_EQ(win[1].word, "c");
  EXPECT_EQ(win[2].word, "b");

  EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, 3, 3, win), 1);  // only "a" left
  EXPECT_EQ(win[0].word, "a");
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
  for (const char* w : {"a", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  EXPECT_TRUE(FlashcardDeck::removeAt(cachePath, 1));  // oldest=0 -> removes "b"
  EXPECT_EQ(FlashcardDeck::count(cachePath), 2);
  EXPECT_EQ(at(0).word, "c");
  EXPECT_EQ(at(1).word, "a");
  EXPECT_FALSE(FlashcardDeck::removeAt(cachePath, 5));  // out of range
}

TEST_F(FlashcardDeckTest, RemoveByWordDropsMatchingRowPreservesOrder) {
  for (const char* w : {"a", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  EXPECT_TRUE(FlashcardDeck::remove(cachePath, "b"));
  EXPECT_EQ(FlashcardDeck::count(cachePath), 2);
  EXPECT_EQ(at(0).word, "c");  // newest-first order otherwise unchanged
  EXPECT_EQ(at(1).word, "a");
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

  // Order unchanged: alpha still oldest, beta still newest.
  EXPECT_EQ(at(1).word, "alpha");
  EXPECT_EQ(at(1).box, 1);
  EXPECT_EQ(at(1).dueDay, 100u + FlashcardDeck::BOX_INTERVAL_DAYS[1]);
  EXPECT_EQ(at(1).excerpt, "alpha ctx");  // excerpt preserved
  EXPECT_EQ(at(0).word, "beta");
  EXPECT_EQ(at(0).box, 0);  // untouched
}

TEST_F(FlashcardDeckTest, GradeAbsentWordIsNoOp) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  EXPECT_FALSE(FlashcardDeck::grade(cachePath, "ghost", true, 100));
  EXPECT_EQ(at(0).box, 0);
}

// --------------------------------------------------------------------------
// buildSession
// --------------------------------------------------------------------------

TEST_F(FlashcardDeckTest, BuildSessionAllNewSelectsNewestFirst) {
  for (const char* w : {"a", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  uint16_t out[8];
  // All cards are new (dueDay==0). DueFirst falls through to the New tier.
  EXPECT_EQ(FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, /*today=*/10, 8, out), 4);
  EXPECT_EQ(out[0], 0);  // d (newest)
  EXPECT_EQ(out[1], 1);  // c
  EXPECT_EQ(out[2], 2);  // b
  EXPECT_EQ(out[3], 3);  // a
}

TEST_F(FlashcardDeckTest, BuildSessionDueFirstOrdersScheduledThenNew) {
  for (const char* w : {"a", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  // Schedule b and c as due (graded at day 5 -> due day 7).
  FlashcardDeck::grade(cachePath, "b", true, 5);
  FlashcardDeck::grade(cachePath, "c", true, 5);

  // newest-first indices: d=0, c=1, b=2, a=3.
  uint16_t out[8];
  EXPECT_EQ(FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, /*today=*/10, 8, out), 4);
  // Scheduled-due tier newest-first: c(1), b(2). Then new tier: d(0), a(3).
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[1], 2);
  EXPECT_EQ(out[2], 0);
  EXPECT_EQ(out[3], 3);
}

TEST_F(FlashcardDeckTest, BuildSessionRespectsCap) {
  for (const char* w : {"a", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::grade(cachePath, "b", true, 5);
  FlashcardDeck::grade(cachePath, "c", true, 5);
  uint16_t out[1];
  EXPECT_EQ(FlashcardDeck::buildSession(cachePath, SessionScope::DueFirst, 10, 1, out), 1);
  EXPECT_EQ(out[0], 1);  // most-recent scheduled-due card (c)
}

TEST_F(FlashcardDeckTest, BuildSessionExcludesRetired) {
  for (const char* w : {"a", "b"}) FlashcardDeck::enroll(cachePath, w, "");
  // Graduate "a": 6 consecutive correct.
  for (int i = 0; i < 6; i++) FlashcardDeck::grade(cachePath, "a", true, 1);
  EXPECT_TRUE(FlashcardDeck::isMastered(at(1).box));

  uint16_t out[8];
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::AllShuffled, 0, 8, out);
  EXPECT_EQ(n, 1);       // only "b" remains schedulable
  EXPECT_EQ(out[0], 0);  // b is newest
}

TEST_F(FlashcardDeckTest, BuildSessionClockUnavailableFallsBackToAll) {
  for (const char* w : {"a", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
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
  for (const char* w : {"a", "b", "c", "d", "e", "f"}) FlashcardDeck::enroll(cachePath, w, "");
  for (int i = 0; i < 6; i++) FlashcardDeck::grade(cachePath, "a", true, 1);  // graduate a -> RETIRED
  FlashcardDeck::grade(cachePath, "b", true, 5);                              // box1, due 7
  FlashcardDeck::grade(cachePath, "d", true, 5);                              // box1, due 7
  FlashcardDeck::grade(cachePath, "f", true, 10);                             // box1, due 12 (future)
  // c, e remain new (box0, due0).

  const FlashcardDeck::Stats s = FlashcardDeck::computeStats(cachePath, /*today=*/10);
  EXPECT_EQ(s.total, 6);
  EXPECT_EQ(s.mastered, 1);
  EXPECT_EQ(s.boxHist[0], 2);    // c, e new
  EXPECT_EQ(s.boxHist[1], 3);    // b, d, f
  EXPECT_EQ(s.due, 4);           // c, e (new) + b, d (due 7 <= 10)
  EXPECT_EQ(s.nextDueDay, 12u);  // f is the only future-scheduled card
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
  for (const char* w : {"a", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  EXPECT_TRUE(FlashcardDeck::suspend(cachePath, "b"));  // middle card
  EXPECT_EQ(at(0).word, "c");
  EXPECT_EQ(at(1).word, "b");  // still in place, just suspended
  EXPECT_EQ(at(1).box, FlashcardDeck::SUSPENDED);
  EXPECT_EQ(at(2).word, "a");
}

TEST_F(FlashcardDeckTest, SuspendAbsentWordIsNoOp) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx");
  EXPECT_FALSE(FlashcardDeck::suspend(cachePath, "ghost"));
  EXPECT_EQ(at(0).box, 0);
}

TEST_F(FlashcardDeckTest, UnsuspendRestoresAsNewCard) {
  FlashcardDeck::enroll(cachePath, "alpha", "ctx", "Ch1");
  FlashcardDeck::grade(cachePath, "alpha", true, 5);  // box1
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
  for (const char* w : {"a", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::suspend(cachePath, "b");

  const FlashcardDeck::Stats s = FlashcardDeck::computeStats(cachePath, /*today=*/10);
  EXPECT_EQ(s.total, 3);
  EXPECT_EQ(s.suspended, 1);
  EXPECT_EQ(s.mastered, 0);
  EXPECT_EQ(s.boxHist[0], 2);  // a, c -- suspended b not in the histogram
  EXPECT_EQ(s.due, 2);         // only a, c are due
}

TEST_F(FlashcardDeckTest, BuildSessionExcludesSuspended) {
  for (const char* w : {"a", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  FlashcardDeck::suspend(cachePath, "b");
  uint16_t out[8];
  // newest-first: c=0, b=1, a=2. Suspended b is dropped from normal scopes.
  const int n = FlashcardDeck::buildSession(cachePath, SessionScope::AllShuffled, 0, 8, out);
  EXPECT_EQ(n, 2);
  EXPECT_EQ(out[0], 0);  // c
  EXPECT_EQ(out[1], 2);  // a
}

TEST_F(FlashcardDeckTest, BuildSessionSuspendedScopeSelectsOnlySuspended) {
  for (const char* w : {"a", "b", "c", "d"}) FlashcardDeck::enroll(cachePath, w, "");
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
  EXPECT_EQ(cat.box, 1);

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

  // B grades "cat" up to box 4 (its own schedule).
  for (int i = 0; i < 4; i++) FlashcardDeck::grade(B, "cat", /*correct=*/true, /*today=*/10);
  FlashcardDeck::Entry e;
  ASSERT_TRUE(findCard(B, "cat", e));
  ASSERT_EQ(e.box, 4);
  const uint32_t dueBefore = e.dueDay;

  // A re-enrolls "cat" with new content (higher version) and syncs to B.
  FlashcardDeck::enroll(cachePath, "cat", "updated ctx", "Ch9");
  syncRound(cachePath, B);

  ASSERT_TRUE(findCard(B, "cat", e));
  EXPECT_EQ(e.box, 4);                  // schedule preserved
  EXPECT_EQ(e.dueDay, dueBefore);       // schedule preserved
  EXPECT_EQ(e.excerpt, "updated ctx");  // content updated
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

}  // namespace
