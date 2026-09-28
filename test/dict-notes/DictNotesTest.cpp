#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "util/DictNotes.h"

namespace {

// Each case exercises one failure mode the store or the matcher can have on its own; the
// gesture that produces a note is device-side and not modeled here.
class DictNotesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Notes live at an absolute device path now (/.crosspoint/dictnotes/<hash>.txt). The shared
    // stub splices those under a root, so each test gets its own tree without writing to /.
    root_ = ::testing::TempDir() + "dict_notes_" + ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(root_);  // TempDir is shared across runs
    std::filesystem::create_directories(root_);
    Storage.setRoot(root_);
  }

  void TearDown() override { Storage.setRoot(""); }

  std::string notesFile(uint32_t dictHash) const {
    char name[16];
    snprintf(name, sizeof(name), "/%08lx.txt", static_cast<unsigned long>(dictHash));
    return root_ + DictNotes::NOTES_DIR + name;
  }

  std::string rawFileContents(uint32_t dictHash) const {
    std::ifstream in(notesFile(dictHash), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  std::string root_;
};

constexpr uint32_t kDictA = 0x1111u;
constexpr uint32_t kDictB = 0x2222u;

TEST_F(DictNotesTest, LoadsWindowNewestFirstAndSuppressesDuplicates) {
  ASSERT_TRUE(DictNotes::add("run", kDictA, "to move at speed"));
  ASSERT_TRUE(DictNotes::add("run", kDictA, "a series of performances"));
  // The same text again is not a second note: re-highlighting a sentence must not fill the list.
  ASSERT_TRUE(DictNotes::add("run", kDictA, "to move at speed"));
  EXPECT_EQ(DictNotes::count(kDictA), 2);

  DictNotes::Note window[4];
  ASSERT_EQ(DictNotes::loadWindow(kDictA, 0, 4, window), 2);
  EXPECT_EQ(window[0].text, "a series of performances");  // newest first
  EXPECT_EQ(window[1].text, "to move at speed");
  EXPECT_EQ(window[0].word, "run");
  EXPECT_EQ(window[0].dictHash, kDictA);
}

TEST_F(DictNotesTest, TextKeepsAnEmbeddedPipe) {
  // text is the REMAINDER of the line, so a '|' inside it is not a field break.
  ASSERT_TRUE(DictNotes::add("bar", kDictA, "a|b|c pipe organ"));
  DictNotes::Note window[2];
  ASSERT_EQ(DictNotes::loadWindow(kDictA, 0, 2, window), 1);
  EXPECT_EQ(window[0].text, "a|b|c pipe organ");
  EXPECT_EQ(window[0].word, "bar");
}

TEST_F(DictNotesTest, NewlinesCollapseSoOneNoteIsOneLine) {
  ASSERT_TRUE(DictNotes::add("wrap", kDictA, "first half\nsecond half"));
  const std::string raw = rawFileContents(kDictA);
  EXPECT_EQ(raw.find('\n'), raw.size() - 1) << "only the line terminator may be a newline";
  DictNotes::Note window[2];
  ASSERT_EQ(DictNotes::loadWindow(kDictA, 0, 2, window), 1);
  EXPECT_EQ(window[0].text, "first half second half");
}

TEST_F(DictNotesTest, RemoveAtDropsOnlyTheNamedNote) {
  ASSERT_TRUE(DictNotes::add("a", kDictA, "oldest"));
  ASSERT_TRUE(DictNotes::add("b", kDictA, "middle"));
  ASSERT_TRUE(DictNotes::add("c", kDictA, "newest"));

  ASSERT_TRUE(DictNotes::removeAt(kDictA, 1));  // newest-first index 1 == "middle"

  DictNotes::Note window[4];
  ASSERT_EQ(DictNotes::loadWindow(kDictA, 0, 4, window), 2);
  EXPECT_EQ(window[0].text, "newest");
  EXPECT_EQ(window[1].text, "oldest");
}

TEST_F(DictNotesTest, CapEvictsTheOldestNotTheNewest) {
  for (int i = 0; i < DictNotes::MAX_NOTES + 2; i++) {
    ASSERT_TRUE(DictNotes::add("w", kDictA, "note " + std::to_string(i)));
  }
  EXPECT_EQ(DictNotes::count(kDictA), DictNotes::MAX_NOTES);

  DictNotes::Note window[2];
  ASSERT_EQ(DictNotes::loadWindow(kDictA, 0, 2, window), 2);
  EXPECT_EQ(window[0].text, "note " + std::to_string(DictNotes::MAX_NOTES + 1));

  DictNotes::Note oldest[1];
  ASSERT_EQ(DictNotes::loadWindow(kDictA, DictNotes::MAX_NOTES - 1, 1, oldest), 1);
  EXPECT_EQ(oldest[0].text, "note 2");  // 0 and 1 were evicted
}

TEST_F(DictNotesTest, MarksAreScopedToOneWordAndOneDictionary) {
  ASSERT_TRUE(DictNotes::add("run", kDictA, "wanted"));
  ASSERT_TRUE(DictNotes::add("run", kDictB, "other dictionary"));
  ASSERT_TRUE(DictNotes::add("walk", kDictA, "other word"));

  DictNotes::Mark marks[DictNotes::MAX_MARKS];
  ASSERT_EQ(DictNotes::loadMarksForWord("run", kDictA, marks, DictNotes::MAX_MARKS), 1);
  EXPECT_STREQ(marks[0].text, "wanted");
}

TEST_F(DictNotesTest, EachDictionaryKeepsItsOwnFile) {
  // The scope that matters to the reader: a note is filed under the DICTIONARY, so the book it
  // was taken from is irrelevant, and a second dictionary's notes never mix in.
  ASSERT_TRUE(DictNotes::add("run", kDictA, "from dictionary A"));
  ASSERT_TRUE(DictNotes::add("run", kDictB, "from dictionary B"));

  EXPECT_EQ(DictNotes::count(kDictA), 1);
  EXPECT_EQ(DictNotes::count(kDictB), 1);
  EXPECT_TRUE(std::filesystem::exists(notesFile(kDictA)));
  EXPECT_TRUE(std::filesystem::exists(notesFile(kDictB)));

  DictNotes::Note window[2];
  ASSERT_EQ(DictNotes::loadWindow(kDictB, 0, 2, window), 1);
  EXPECT_EQ(window[0].text, "from dictionary B");
}

TEST_F(DictNotesTest, DeclinesWhenNoDictionaryIsResolved) {
  // dictHash 0 is "unset" everywhere else (DictionaryRegistry::nameHash), and there is no file
  // to name for it -- a note with no dictionary could never be matched back.
  EXPECT_FALSE(DictNotes::add("run", 0, "nowhere to file this"));
  EXPECT_EQ(DictNotes::count(0), 0);
}

TEST_F(DictNotesTest, ListsEveryDictionaryThatHasNotesAndIgnoresStrayFiles) {
  // The case that prompted the picker: notes kept in a dictionary that is no longer active must
  // still be findable, so the listing is what the screen enumerates rather than "the active one".
  ASSERT_TRUE(DictNotes::add("run", kDictA, "from A"));
  ASSERT_TRUE(DictNotes::add("walk", kDictA, "also from A"));
  ASSERT_TRUE(DictNotes::add("run", kDictB, "from B"));

  // Anything in the directory that is not <8 hex>.txt is not a dictionary: a temp file left by an
  // interrupted rewrite, or something dropped there by hand.
  std::filesystem::create_directories(root_ + DictNotes::NOTES_DIR);
  std::ofstream(root_ + DictNotes::NOTES_DIR + "/notes.txt") << "junk\n";
  std::ofstream(root_ + DictNotes::NOTES_DIR + "/00001111.tmp") << "junk\n";

  DictNotes::DictSummary summaries[DictNotes::MAX_DICTS];
  const int n = DictNotes::listDictionaries(summaries, DictNotes::MAX_DICTS);
  ASSERT_EQ(n, 2);

  int countA = 0, countB = 0;
  for (int i = 0; i < n; i++) {
    if (summaries[i].dictHash == kDictA) countA = summaries[i].count;
    if (summaries[i].dictHash == kDictB) countB = summaries[i].count;
  }
  EXPECT_EQ(countA, 2);
  EXPECT_EQ(countB, 1);
}

TEST_F(DictNotesTest, DeletingTheLastNoteDropsTheDictionary) {
  ASSERT_TRUE(DictNotes::add("run", kDictA, "the only note"));
  ASSERT_TRUE(DictNotes::removeAt(kDictA, 0));

  EXPECT_EQ(DictNotes::count(kDictA), 0);
  EXPECT_FALSE(std::filesystem::exists(notesFile(kDictA))) << "an emptied file would list as a dictionary with none";

  DictNotes::DictSummary summaries[DictNotes::MAX_DICTS];
  EXPECT_EQ(DictNotes::listDictionaries(summaries, DictNotes::MAX_DICTS), 0);
}

// --- Matching a note back onto a laid-out page ---------------------------------------------

DictNotes::Segment seg(const char* text) {
  return DictNotes::Segment{text, static_cast<uint16_t>(std::string(text).size())};
}

TEST_F(DictNotesTest, MatchSpansASegmentBreak) {
  // The wrap put "at" at the end of one line and "speed" at the start of the next.
  const DictNotes::Segment segs[] = {seg("to move at"), seg("speed quickly")};
  DictNotes::Span spans[4];
  const int n = DictNotes::findSpans(segs, 2, "at speed", spans, 4);

  ASSERT_EQ(n, 2) << "one span per segment the note touches";
  EXPECT_EQ(spans[0].segIndex, 0);
  EXPECT_EQ(std::string(segs[0].text + spans[0].byteStart, spans[0].byteLen), "at");
  EXPECT_EQ(spans[1].segIndex, 1);
  EXPECT_EQ(std::string(segs[1].text + spans[1].byteStart, spans[1].byteLen), "speed");
}

TEST_F(DictNotesTest, MatchStepsOverAPunctuationOnlyToken) {
  // extractWordsFromLayout drops a token that cleans to nothing, so the saved note never
  // contains the dash the page shows between the two words.
  const DictNotes::Segment segs[] = {seg("a sudden — violent storm")};
  DictNotes::Span spans[4];
  const int n = DictNotes::findSpans(segs, 1, "sudden violent", spans, 4);

  ASSERT_EQ(n, 1);
  EXPECT_EQ(std::string(segs[0].text + spans[0].byteStart, spans[0].byteLen), "sudden — violent");
}

TEST_F(DictNotesTest, MatchDoesNotFireInsideALongerWord) {
  const DictNotes::Segment segs[] = {seg("a mansion stood there")};
  DictNotes::Span spans[4];
  EXPECT_EQ(DictNotes::findSpans(segs, 1, "a man", spans, 4), 0);
}

TEST_F(DictNotesTest, MatchesACjkRunWrittenWithoutSpaces) {
  // CJK is laid out with no spaces, so the note and the page agree on one token.
  const DictNotes::Segment segs[] = {seg("中国 的 首都")};
  DictNotes::Span spans[4];
  const int n = DictNotes::findSpans(segs, 1, "中国 的", spans, 4);

  ASSERT_EQ(n, 1);
  EXPECT_EQ(std::string(segs[0].text + spans[0].byteStart, spans[0].byteLen), "中国 的");
}

TEST_F(DictNotesTest, MatchResumesAfterAFalseStart) {
  // The first "the" opens a run that breaks; the occurrence starting at the second must
  // still be found.
  const DictNotes::Segment segs[] = {seg("the cat the dog")};
  DictNotes::Span spans[4];
  const int n = DictNotes::findSpans(segs, 1, "the dog", spans, 4);

  ASSERT_EQ(n, 1);
  EXPECT_EQ(std::string(segs[0].text + spans[0].byteStart, spans[0].byteLen), "the dog");
}

}  // namespace
