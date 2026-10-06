// Host litmus for LookupMarks — the resident table that tells the reader which looked-up word
// sits on which page, and the hash rule the page walk matches tokens against.
//
// The drawing itself needs a GfxRenderer and is device-tested; everything decided before a
// pixel is touched is here.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "util/LookupMarks.h"

namespace {

uint32_t hash(const std::string& s) { return LookupMarks::hashWord(s.c_str(), s.size()); }

// Accumulate a word one page token at a time, the way the CJK run matcher does.
uint32_t hashRun(const std::initializer_list<const char*> tokens, uint16_t& len) {
  uint32_t h = LookupMarks::FNV_OFFSET;
  len = 0;
  for (const char* t : tokens) h = LookupMarks::hashAppend(h, t, std::strlen(t), &len);
  return h;
}

class LookupMarksTest : public ::testing::Test {
 protected:
  void SetUp() override { LookupMarks::getInstance().clear(); }
  void TearDown() override { LookupMarks::getInstance().clear(); }
};

// The card holds whatever cleanWord() left of the word; the page holds it as typeset, commas
// and capitals and all. Normalisation is what makes those the same word.
TEST_F(LookupMarksTest, HashIgnoresCaseAndAsciiPunctuation) {
  EXPECT_EQ(hash("fork"), hash("Fork"));
  EXPECT_EQ(hash("fork"), hash("fork,"));
  EXPECT_EQ(hash("fork"), hash("\"fork\""));
  EXPECT_EQ(hash("didn't"), hash("didnt"));
  EXPECT_NE(hash("fork"), hash("forks"));
  EXPECT_NE(hash("helix"), hash("helices"));
}

// Non-ASCII bytes carry identity and are hashed verbatim — case folding is ASCII-only, so an
// accented word is still distinct from its unaccented spelling.
TEST_F(LookupMarksTest, HashKeepsNonAsciiBytes) {
  EXPECT_NE(hash("café"), hash("cafe"));
  EXPECT_EQ(hash("café"), hash("Café"));
}

// Accumulating token by token has to land on exactly the hash (and length) of the whole word,
// or a CJK run could never match.
TEST_F(LookupMarksTest, RunAccumulationEqualsTheWholeWord) {
  uint16_t len = 0;
  const uint32_t run = hashRun({"中", "国", "人"}, len);
  EXPECT_EQ(run, hash("中国人"));
  EXPECT_EQ(len, 9);  // three 3-byte codepoints

  // A run that has taken the wrong characters does not collide with the right word.
  uint16_t wrongLen = 0;
  EXPECT_NE(hashRun({"中", "中", "国"}, wrongLen), hash("中国人"));
  EXPECT_EQ(wrongLen, 9);  // same length, different word: the length test alone is not enough
}

TEST_F(LookupMarksTest, AddRecordsWordHashHeadHashAndLength) {
  auto& marks = LookupMarks::getInstance();
  ASSERT_TRUE(marks.add("中国人", 9, "第一章", 9, 3, 12));

  const LookupMarks::Mark* out[4];
  ASSERT_EQ(marks.collectForPage(LookupMarks::hashChapter("第一章", 9), 3, 12, out, 4), 1);
  EXPECT_EQ(out[0]->wordHash, hash("中国人"));
  EXPECT_EQ(out[0]->headHash, hash("中"));  // the run opens on the first character
  EXPECT_EQ(out[0]->byteLen, 9);
}

// A Latin word is one page token, so its head hash is just itself and the run path is unused.
TEST_F(LookupMarksTest, LatinWordHeadHashIsItsFirstLetter) {
  auto& marks = LookupMarks::getInstance();
  ASSERT_TRUE(marks.add("helix", 5, "Bident", 6, 12, 27));
  const LookupMarks::Mark* out[4];
  ASSERT_EQ(marks.collectForPage(LookupMarks::hashChapter("Bident", 6), 12, 27, out, 4), 1);
  EXPECT_EQ(out[0]->wordHash, hash("helix"));
  EXPECT_EQ(out[0]->headHash, hash("h"));
  EXPECT_EQ(out[0]->byteLen, 5);
}

// A card with no page token has no anchor and must not be stored: it would otherwise have to
// be matched against every page.
TEST_F(LookupMarksTest, UnanchoredCardsAreRejected) {
  auto& marks = LookupMarks::getInstance();
  EXPECT_FALSE(marks.add("vermiform", 9, "Bident", 6, 0, 0));
  EXPECT_FALSE(marks.add("", 0, "Bident", 6, 3, 12));
  EXPECT_FALSE(marks.add("...", 3, "Bident", 6, 3, 12));  // nothing survives normalisation
  EXPECT_TRUE(marks.empty());
}

TEST_F(LookupMarksTest, CollectMatchesOnlyItsOwnPage) {
  auto& marks = LookupMarks::getInstance();
  const uint32_t ch = LookupMarks::hashChapter("Bident", 6);
  ASSERT_TRUE(marks.add("pews", 4, "Bident", 6, 11, 27));
  ASSERT_TRUE(marks.add("helix", 5, "Bident", 6, 12, 27));

  const LookupMarks::Mark* out[4];
  EXPECT_EQ(marks.collectForPage(ch, 11, 27, out, 4), 1);
  EXPECT_EQ(out[0]->wordHash, hash("pews"));
  EXPECT_EQ(marks.collectForPage(ch, 13, 27, out, 4), 0);                                    // no card here
  EXPECT_EQ(marks.collectForPage(LookupMarks::hashChapter("Other", 5), 11, 27, out, 4), 0);  // other chapter
}

// After a re-layout (status bar toggle, font, margins, another device) the chapter has a
// different page count. The mark follows its position in the chapter onto the pages now covering
// it, widened by RELAYOUT_SLACK_PAGES because the text does not move evenly.
TEST_F(LookupMarksTest, RepaginationMapsTheMarkByChapterPosition) {
  auto& marks = LookupMarks::getInstance();
  ASSERT_TRUE(marks.add("pews", 4, "Bident", 6, 11, 27));  // slice [10/27, 11/27) = [.370, .407)
  const LookupMarks::Mark* out[4];
  const uint32_t ch = LookupMarks::hashChapter("Bident", 6);
  EXPECT_EQ(marks.collectForPage(ch, 12, 31, out, 4), 1);  // [.355, .387) overlaps
  EXPECT_EQ(marks.collectForPage(ch, 13, 31, out, 4), 1);  // [.387, .419) overlaps
  // Within RELAYOUT_SLACK_PAGES (2) of those: pages 10..15.
  EXPECT_EQ(marks.collectForPage(ch, 10, 31, out, 4), 1);
  EXPECT_EQ(marks.collectForPage(ch, 15, 31, out, 4), 1);
  EXPECT_EQ(marks.collectForPage(ch, 9, 31, out, 4), 0);
  EXPECT_EQ(marks.collectForPage(ch, 16, 31, out, 4), 0);
  // Its own page number always matches: a total recorded mid-build was an estimate, so the
  // count drifting under an unchanged layout must not lose the mark.
  EXPECT_EQ(marks.collectForPage(ch, 11, 31, out, 4), 1);
}

// The table is a fixed allocation; past its size the oldest lookups fall out, not the newest.
TEST_F(LookupMarksTest, OverflowKeepsTheNewestLookups) {
  auto& marks = LookupMarks::getInstance();
  const uint32_t ch = LookupMarks::hashChapter("Bident", 6);
  for (int i = 0; i < LookupMarks::MAX_MARKS + 5; i++) {
    const std::string word = "word" + std::to_string(i);
    ASSERT_TRUE(marks.add(word.c_str(), static_cast<int>(word.size()), "Bident", 6, 1, 1));
  }
  EXPECT_EQ(marks.size(), LookupMarks::MAX_MARKS);

  const LookupMarks::Mark* out[LookupMarks::MAX_MARKS];
  const int n = marks.collectForPage(ch, 1, 1, out, LookupMarks::MAX_MARKS);
  ASSERT_EQ(n, LookupMarks::MAX_MARKS);
  bool hasNewest = false, hasOldest = false;
  for (int i = 0; i < n; i++) {
    if (out[i]->wordHash == hash("word" + std::to_string(LookupMarks::MAX_MARKS + 4))) hasNewest = true;
    if (out[i]->wordHash == hash("word0")) hasOldest = true;
  }
  EXPECT_TRUE(hasNewest);
  EXPECT_FALSE(hasOldest);
}

// A chapter with no TOC entry stores an empty title. The reader has to key such a page with
// the empty hash, not with 0 — getting that wrong silently loses every mark in the chapter.
TEST_F(LookupMarksTest, UntitledChapterKeysOnTheEmptyHash) {
  auto& marks = LookupMarks::getInstance();
  ASSERT_TRUE(marks.add("helix", 5, nullptr, 0, 12, 27));
  const LookupMarks::Mark* out[4];
  EXPECT_EQ(marks.collectForPage(LookupMarks::hashChapter(nullptr, 0), 12, 27, out, 4), 1);
  EXPECT_EQ(marks.collectForPage(0, 12, 27, out, 4), 0);
  EXPECT_EQ(LookupMarks::hashChapter(nullptr, 0), LookupMarks::hashChapter("", 0));
}

// --------------------------------------------------------------------------------------
// The run matcher (LookupMarks::step)
//
// This is the predicate BOTH page walks drive: the one that inks the underline
// (PageMarks::drawForPage) and the one that answers which word a hold landed on
// (PageMarks::lookupMarkAtPoint). If they ever disagreed, the page's hold menu would offer to
// delete a card for a word that is not underlined, or refuse one that is. The walks are
// device-tested; the arithmetic they share is pinned here.
// --------------------------------------------------------------------------------------

using Step = LookupMarks::Step;

// Feed one page token to a mark, the way both walks do.
Step feed(const LookupMarks::Mark& m, LookupMarks::RunState& r, bool isCjk, const char* token, int16_t rowY = 100) {
  uint16_t len = 0;
  const uint32_t h = LookupMarks::hashAppend(LookupMarks::FNV_OFFSET, token, std::strlen(token), &len);
  return LookupMarks::step(m, r, isCjk, h, len, token, std::strlen(token), rowY);
}

// Build a mark the way add() does, without going through the table.
LookupMarks::Mark markFor(const std::string& word, const std::string& head) {
  LookupMarks::Mark m{};
  uint16_t len = 0;
  m.wordHash = LookupMarks::hashAppend(LookupMarks::FNV_OFFSET, word.c_str(), word.size(), &len);
  m.byteLen = len;
  m.headHash = LookupMarks::hashWord(head.c_str(), head.size());
  return m;
}

// A Latin word is one page token: it settles on that token or not at all, and never touches the
// run state.
TEST_F(LookupMarksTest, StepMatchesWholeTokenWithoutOpeningARun) {
  const LookupMarks::Mark m = markFor("forest", "f");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/false, "canopy"), Step::None);
  EXPECT_FALSE(r.open);
  EXPECT_EQ(feed(m, r, /*isCjk=*/false, "Forest,"), Step::MatchedToken);
  EXPECT_FALSE(r.open);
}

// A longer word that merely STARTS with the mark must not match: byteLen is what stops
// "forest" underlining the "forest" inside "forestry".
TEST_F(LookupMarksTest, StepRejectsALongerTokenSharingThePrefix) {
  const LookupMarks::Mark m = markFor("forest", "f");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/false, "forestry"), Step::None);
}

// CJK lays out one token per character, so a two-character word is a RUN: opened on the head
// character, completed by the next. The caller seeds its span on Opened and inks it on
// MatchedRun.
TEST_F(LookupMarksTest, StepAccumulatesACjkRun) {
  const LookupMarks::Mark m = markFor("森林", "森");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "森"), Step::Opened);
  EXPECT_TRUE(r.open);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "林"), Step::MatchedRun);
  EXPECT_FALSE(r.open);  // consumed
}

// A one-character CJK word opens and completes on the SAME token, so there is no seeded span for
// the caller to ink — it has to be told the span is this token. This is the case that makes
// MatchedToken and MatchedRun separate states rather than one "Matched".
TEST_F(LookupMarksTest, StepReportsASingleCharacterCjkWordAsAToken) {
  const LookupMarks::Mark m = markFor("森", "森");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "森"), Step::MatchedToken);
  EXPECT_FALSE(r.open);
}

// A run that reaches the mark's length with different bytes is done, not merely paused: leaving
// it open would let the next character complete it and underline the wrong span.
TEST_F(LookupMarksTest, StepClosesARunThatOvershootsOrMismatches) {
  const LookupMarks::Mark m = markFor("森林", "森");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "森"), Step::Opened);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "岛"), Step::Extended);  // 森岛 != 森林
  EXPECT_FALSE(r.open);
  // The next head character starts a fresh run rather than extending the dead one.
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "森"), Step::Opened);
}

// A word wrapped between two lines is still one word: the run crosses the break, and the step
// that crossed it says so, so the caller can close the first segment and start the second.
TEST_F(LookupMarksTest, StepCarriesARunAcrossOneLineBreak) {
  const LookupMarks::Mark m = markFor("邪门歪道", "邪");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "邪", /*rowY=*/100), Step::Opened);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "门", /*rowY=*/100), Step::Extended);
  EXPECT_FALSE(r.wrappedHere);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "歪", /*rowY=*/140), Step::Extended);
  EXPECT_TRUE(r.wrappedHere);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "道。", /*rowY=*/140), Step::MatchedRun);
  EXPECT_FALSE(r.wrappedHere);
}

// Only one break: a run reaching a third row is abandoned, and the next run starts unwrapped.
TEST_F(LookupMarksTest, StepAbandonsARunOnASecondLineBreak) {
  const LookupMarks::Mark m = markFor("图书馆", "图");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "图", /*rowY=*/100), Step::Opened);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "书", /*rowY=*/140), Step::Extended);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "馆", /*rowY=*/180), Step::None);
  EXPECT_FALSE(r.open);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "图", /*rowY=*/180), Step::Opened);
  EXPECT_FALSE(r.wrapped);
}

// Layout keeps CJK closing punctuation on the character before it and opening punctuation on
// the one after, so the page token is "边，" or "「沾". Neither may change the word's identity.
TEST_F(LookupMarksTest, StepIgnoresCjkPunctuationGluedToAToken) {
  const LookupMarks::Mark m = markFor("沾边", "沾");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "「沾"), Step::Opened);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "边，"), Step::MatchedRun);
  EXPECT_EQ(LookupMarks::hashWord("“夫妇”", std::strlen("“夫妇”")), LookupMarks::hashWord("夫妇", 6));
}

// A three-character word needs both intermediate steps before it settles.
TEST_F(LookupMarksTest, StepCompletesAThreeCharacterRun) {
  const LookupMarks::Mark m = markFor("图书馆", "图");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "图"), Step::Opened);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "书"), Step::Extended);
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "馆"), Step::MatchedRun);
}

// A run only ever opens on the mark's head character, so a word starting elsewhere is skipped
// outright — this is what keeps two marked words sharing a character on one page apart.
TEST_F(LookupMarksTest, StepOnlyOpensOnTheHeadCharacter) {
  const LookupMarks::Mark m = markFor("森林", "森");
  LookupMarks::RunState r;
  EXPECT_EQ(feed(m, r, /*isCjk=*/true, "林"), Step::None);
  EXPECT_FALSE(r.open);
}

}  // namespace
