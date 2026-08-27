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
  EXPECT_EQ(marks.collectForPage(ch, 13, 27, out, 4), 0);                                 // no card here
  EXPECT_EQ(marks.collectForPage(LookupMarks::hashChapter("Other", 5), 11, 27, out, 4), 0);  // other chapter
}

// Page numbers only describe the pagination that produced them. After a font or margin change
// the chapter has a different page count, and every stored page number is meaningless — better
// no mark than a mark on the wrong word.
TEST_F(LookupMarksTest, RepaginationDropsTheMarks) {
  auto& marks = LookupMarks::getInstance();
  ASSERT_TRUE(marks.add("pews", 4, "Bident", 6, 11, 27));
  const LookupMarks::Mark* out[4];
  EXPECT_EQ(marks.collectForPage(LookupMarks::hashChapter("Bident", 6), 11, 31, out, 4), 0);
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

}  // namespace
