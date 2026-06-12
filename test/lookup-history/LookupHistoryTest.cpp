#include "util/LookupHistory.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

namespace {

using Status = LookupHistory::Status;

class LookupHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cachePath = ::testing::TempDir() + "lookup_history_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(cachePath);
    // TempDir is shared across runs; start from a clean slate.
    std::remove(historyFile().c_str());
    std::remove(tmpFile().c_str());
    SETTINGS.lookupHistoryCap = 100;
  }

  std::string historyFile() const { return cachePath + "/dictionary_history.txt"; }
  std::string tmpFile() const { return cachePath + "/dictionary_history.tmp"; }

  std::string rawFileContents() const {
    std::ifstream in(historyFile(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  void writeRawFile(const std::string& contents) const {
    std::ofstream out(historyFile(), std::ios::binary);
    out << contents;
  }

  std::string cachePath;
};

TEST_F(LookupHistoryTest, LoadFromMissingFileIsEmpty) {
  EXPECT_TRUE(LookupHistory::load(cachePath).empty());
}

TEST_F(LookupHistoryTest, AddWordCreatesFileWithSingleEntry) {
  EXPECT_EQ(LookupHistory::addWord(cachePath, "alpha", Status::Direct), 1);
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "alpha");
  EXPECT_EQ(entries[0].status, Status::Direct);
  EXPECT_EQ(rawFileContents(), "alpha|D\n");
}

TEST_F(LookupHistoryTest, LoadReturnsNewestFirst) {
  LookupHistory::addWord(cachePath, "alpha", Status::Direct);
  LookupHistory::addWord(cachePath, "beta", Status::Stem);
  LookupHistory::addWord(cachePath, "gamma", Status::NotFound);
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].word, "gamma");
  EXPECT_EQ(entries[0].status, Status::NotFound);
  EXPECT_EQ(entries[1].word, "beta");
  EXPECT_EQ(entries[1].status, Status::Stem);
  EXPECT_EQ(entries[2].word, "alpha");
  EXPECT_EQ(entries[2].status, Status::Direct);
}

TEST_F(LookupHistoryTest, DedupMovesWordToNewestAndRefreshesStatus) {
  LookupHistory::addWord(cachePath, "alpha", Status::Direct);
  LookupHistory::addWord(cachePath, "beta", Status::Stem);
  EXPECT_EQ(LookupHistory::addWord(cachePath, "alpha", Status::Suggestion), 2);
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "alpha");
  EXPECT_EQ(entries[0].status, Status::Suggestion);
  EXPECT_EQ(entries[1].word, "beta");
}

TEST_F(LookupHistoryTest, DedupDoesNotMatchPrefixOrSuperstring) {
  LookupHistory::addWord(cachePath, "cat", Status::Direct);
  LookupHistory::addWord(cachePath, "catalog", Status::Direct);
  EXPECT_EQ(LookupHistory::addWord(cachePath, "cat", Status::Stem), 2);
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "cat");
  EXPECT_EQ(entries[1].word, "catalog");
}

TEST_F(LookupHistoryTest, EvictsOldestWhenOverCap) {
  SETTINGS.lookupHistoryCap = 3;
  LookupHistory::addWord(cachePath, "a", Status::Direct);
  LookupHistory::addWord(cachePath, "b", Status::Direct);
  LookupHistory::addWord(cachePath, "c", Status::Direct);
  EXPECT_EQ(LookupHistory::addWord(cachePath, "d", Status::Direct), 3);
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].word, "d");
  EXPECT_EQ(entries[1].word, "c");
  EXPECT_EQ(entries[2].word, "b");
}

TEST_F(LookupHistoryTest, DedupAtCapDoesNotEvict) {
  SETTINGS.lookupHistoryCap = 2;
  LookupHistory::addWord(cachePath, "a", Status::Direct);
  LookupHistory::addWord(cachePath, "b", Status::Direct);
  EXPECT_EQ(LookupHistory::addWord(cachePath, "a", Status::Stem), 2);
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "a");
  EXPECT_EQ(entries[1].word, "b");
}

TEST_F(LookupHistoryTest, GetWordNewestFirst) {
  LookupHistory::addWord(cachePath, "alpha", Status::Direct);
  LookupHistory::addWord(cachePath, "beta", Status::Direct);
  LookupHistory::addWord(cachePath, "gamma", Status::Direct);
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, 0), "gamma");
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, 1), "beta");
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, 2), "alpha");
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, 3), "");
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, -1), "");
}

TEST_F(LookupHistoryTest, GetWordNewestFirstOnMissingFile) {
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, 0), "");
}

TEST_F(LookupHistoryTest, RemoveAtMiddleFileIndex) {
  LookupHistory::addWord(cachePath, "alpha", Status::Direct);
  LookupHistory::addWord(cachePath, "beta", Status::Direct);
  LookupHistory::addWord(cachePath, "gamma", Status::Direct);
  EXPECT_TRUE(LookupHistory::removeAt(cachePath, 1));  // oldest=0, so removes "beta"
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "gamma");
  EXPECT_EQ(entries[1].word, "alpha");
}

TEST_F(LookupHistoryTest, RemoveAtOutOfRange) {
  LookupHistory::addWord(cachePath, "alpha", Status::Direct);
  EXPECT_FALSE(LookupHistory::removeAt(cachePath, 1));
  EXPECT_FALSE(LookupHistory::removeAt(cachePath, -1));
  EXPECT_EQ(LookupHistory::load(cachePath).size(), 1u);
}

TEST_F(LookupHistoryTest, LegacyLineWithoutStatusParsesAsNotFound) {
  writeRawFile("plainword\nnewer|D\n");
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "newer");
  EXPECT_EQ(entries[1].word, "plainword");
  EXPECT_EQ(entries[1].status, Status::NotFound);
  // Dedup must match the legacy line too.
  EXPECT_EQ(LookupHistory::addWord(cachePath, "plainword", Status::Direct), 2);
  const auto after = LookupHistory::load(cachePath);
  EXPECT_EQ(after[0].word, "plainword");
  EXPECT_EQ(after[0].status, Status::Direct);
}

TEST_F(LookupHistoryTest, CrlfLineEndingsParse) {
  writeRawFile("alpha|D\r\nbeta|T\r\n");
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "beta");
  EXPECT_EQ(entries[0].status, Status::Stem);
  EXPECT_EQ(entries[1].word, "alpha");
}

TEST_F(LookupHistoryTest, AddWordIfShortCircuits) {
  LookupHistory::addWordIf(cachePath, "alpha", Status::Direct, false);
  LookupHistory::addWordIf(cachePath, "", Status::Direct, true);
  LookupHistory::addWordIf("", "alpha", Status::Direct, true);
  EXPECT_TRUE(LookupHistory::load(cachePath).empty());
  LookupHistory::addWordIf(cachePath, "alpha", Status::Direct, true);
  EXPECT_EQ(LookupHistory::load(cachePath).size(), 1u);
}

TEST_F(LookupHistoryTest, WordContainingPipeKeepsLastSeparator) {
  // '|' in a word splits on the LAST separator -- the suffix becomes the status.
  writeRawFile("odd|word|D\n");
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "odd|word");
  EXPECT_EQ(entries[0].status, Status::Direct);
}

TEST_F(LookupHistoryTest, ManyEntriesRoundTrip) {
  SETTINGS.lookupHistoryCap = 225;
  for (int i = 0; i < 225; i++) {
    char w[16];
    std::snprintf(w, sizeof(w), "word%03d", i);
    LookupHistory::addWord(cachePath, w, Status::Direct);
  }
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 225u);
  EXPECT_EQ(entries[0].word, "word224");
  EXPECT_EQ(entries[224].word, "word000");
  // One more evicts the single oldest.
  EXPECT_EQ(LookupHistory::addWord(cachePath, "overflow", Status::Direct), 225);
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, 0), "overflow");
  EXPECT_EQ(LookupHistory::getWordNewestFirst(cachePath, 224), "word001");
}

}  // namespace
