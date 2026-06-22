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
    // Remove the deck + any temp files, then the dir.
    for (const char* f : {"dictionary_flashcards.txt", "dictionary_flashcards.tmp"}) {
      std::remove((cachePath + "/" + f).c_str());
    }
    std::remove(cachePath.c_str());
  }

  // Read the card for a newest-first index (helper around loadWindow).
  FlashcardDeck::Entry at(int newestIdx) {
    FlashcardDeck::Entry e;
    EXPECT_EQ(FlashcardDeck::loadWindow(cachePath, newestIdx, 1, &e), 1);
    return e;
  }

  std::string cachePath;
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

TEST_F(FlashcardDeckTest, RemoveAtByFileIndex) {
  for (const char* w : {"a", "b", "c"}) FlashcardDeck::enroll(cachePath, w, "");
  EXPECT_TRUE(FlashcardDeck::removeAt(cachePath, 1));  // oldest=0 -> removes "b"
  EXPECT_EQ(FlashcardDeck::count(cachePath), 2);
  EXPECT_EQ(at(0).word, "c");
  EXPECT_EQ(at(1).word, "a");
  EXPECT_FALSE(FlashcardDeck::removeAt(cachePath, 5));  // out of range
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
  EXPECT_EQ(e.box, 0u);       // back in the new pool
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

}  // namespace
