#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "util/LookupHistory.h"

namespace {

using Status = LookupHistory::Status;

class LookupHistoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cachePath =
        ::testing::TempDir() + "lookup_history_" + ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(cachePath);
    // TempDir is shared across runs; start from a clean slate.
    std::remove(historyFile().c_str());
    std::remove(tmpFile().c_str());
    std::remove(tombFile().c_str());
    std::remove(verFile().c_str());
    std::remove(syncFile().c_str());
    SETTINGS.lookupHistoryCap = 100;
  }

  std::string historyFile() const { return cachePath + "/dictionary_history.txt"; }
  std::string tmpFile() const { return cachePath + "/dictionary_history.tmp"; }
  std::string tombFile() const { return cachePath + "/dictionary_history.tomb"; }
  std::string verFile() const { return cachePath + "/dictionary_history.ver"; }
  std::string syncFile() const { return cachePath + "/dictionary_history.sync"; }

  std::string rawFileContents() const {
    std::ifstream in(historyFile(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  void writeRawFile(const std::string& contents) const {
    std::ofstream out(historyFile(), std::ios::binary);
    out << contents;
  }

  std::string syncFileContents() const {
    std::ifstream in(syncFile(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  void writeSyncFile(const std::string& contents) const {
    std::ofstream out(syncFile(), std::ios::binary);
    out << contents;
  }

  // Decode a serialized blob into a printable string for assertions.
  static std::string blobStr(const uint8_t* b, size_t n) { return std::string(reinterpret_cast<const char*>(b), n); }

  // Count occurrences of `needle` in `hay` (non-overlapping).
  static int countOccurrences(const std::string& hay, const std::string& needle) {
    int c = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) c++;
    return c;
  }

  std::string cachePath;
};

TEST_F(LookupHistoryTest, LoadFromMissingFileIsEmpty) { EXPECT_TRUE(LookupHistory::load(cachePath).empty()); }

TEST_F(LookupHistoryTest, AddWordCreatesFileWithSingleEntry) {
  EXPECT_EQ(LookupHistory::addWord(cachePath, "alpha", Status::Direct), 1);
  const auto entries = LookupHistory::load(cachePath);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "alpha");
  EXPECT_EQ(entries[0].status, Status::Direct);
  EXPECT_EQ(rawFileContents(), "alpha|D|1\n");  // word|STATUS|VER (Lamport-versioned)
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

// LookupChain addresses the log by distance-from-newest, so a write has to report whether
// it APPENDED a new word or MOVED one the log already held: the two shift the stored
// indices differently (LookupChain::shifted). The index is taken from the pass addWordVer
// already makes, so this costs no extra read.
TEST_F(LookupHistoryTest, AddWordIfReportsWhetherTheWordWasAlreadyLogged) {
  const auto first = LookupHistory::addWordIf(cachePath, "alpha", Status::Direct, true);
  EXPECT_TRUE(first.wrote);
  EXPECT_EQ(first.prevIndex, -1);  // brand new -> a pure append

  LookupHistory::addWordIf(cachePath, "beta", Status::Direct, true);
  LookupHistory::addWordIf(cachePath, "gamma", Status::Direct, true);  // newest-first: gamma beta alpha

  const auto moved = LookupHistory::addWordIf(cachePath, "alpha", Status::Stem, true);
  EXPECT_TRUE(moved.wrote);
  EXPECT_EQ(moved.prevIndex, 2);  // was the oldest of the three

  const auto alreadyNewest = LookupHistory::addWordIf(cachePath, "alpha", Status::Stem, true);
  EXPECT_TRUE(alreadyNewest.wrote);
  EXPECT_EQ(alreadyNewest.prevIndex, 0);  // re-looking up the word already at the top
  EXPECT_EQ(LookupHistory::load(cachePath).size(), 3u);
}

TEST_F(LookupHistoryTest, AddWordIfReportsSkippedWrites) {
  // Nothing was written, so a caller tracking positions must not re-index.
  EXPECT_FALSE(LookupHistory::addWordIf(cachePath, "alpha", Status::Direct, false).wrote);
  EXPECT_FALSE(LookupHistory::addWordIf(cachePath, "", Status::Direct, true).wrote);
  EXPECT_FALSE(LookupHistory::addWordIf("", "alpha", Status::Direct, true).wrote);
  // The stopword filter lives inside addWordIf, so the log does not move for one either.
  EXPECT_FALSE(LookupHistory::addWordIf(cachePath, "the", Status::Direct, true).wrote);
  EXPECT_TRUE(LookupHistory::load(cachePath).empty());
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

// --- Cross-device delta sync ------------------------------------------------

TEST_F(LookupHistoryTest, SerializeFullEmitsAllHistory) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);  // v1
  LookupHistory::addWord(cachePath, "b", Status::Direct);  // v2
  LookupHistory::addWord(cachePath, "c", Status::Direct);  // v3
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  const size_t n = LookupHistory::serializeBlob(cachePath, buf, sizeof(buf), 0, &st);
  const std::string s = blobStr(buf, n);
  EXPECT_EQ(countOccurrences(s, "H"), 3);
  EXPECT_NE(s.find("a|D|1"), std::string::npos);
  EXPECT_NE(s.find("b|D|2"), std::string::npos);
  EXPECT_NE(s.find("c|D|3"), std::string::npos);
  EXPECT_EQ(st.histCount, 3);
  EXPECT_EQ(st.tombCount, 0);
  EXPECT_EQ(st.maxVer, 3u);
  EXPECT_FALSE(st.truncated);
}

TEST_F(LookupHistoryTest, SerializeDeltaSkipsAtOrBelowWatermark) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);  // v1
  LookupHistory::addWord(cachePath, "b", Status::Direct);  // v2
  LookupHistory::addWord(cachePath, "c", Status::Direct);  // v3
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  const size_t n = LookupHistory::serializeBlob(cachePath, buf, sizeof(buf), 2 /*since*/, &st);
  const std::string s = blobStr(buf, n);
  EXPECT_EQ(st.histCount, 1);  // only v3 > 2
  EXPECT_EQ(st.maxVer, 3u);
  EXPECT_NE(s.find("c|D|3"), std::string::npos);
  EXPECT_EQ(s.find("a|D|1"), std::string::npos);
  EXPECT_EQ(s.find("b|D|2"), std::string::npos);
}

TEST_F(LookupHistoryTest, DeltaIncludesNewTombstoneOnly) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);  // v1
  LookupHistory::addWord(cachePath, "b", Status::Direct);  // v2
  LookupHistory::addWord(cachePath, "c", Status::Direct);  // v3
  EXPECT_TRUE(LookupHistory::removeAt(cachePath, 1));      // delete "b" -> tombstone v4
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  const size_t n = LookupHistory::serializeBlob(cachePath, buf, sizeof(buf), 3 /*since*/, &st);
  const std::string s = blobStr(buf, n);
  EXPECT_EQ(st.tombCount, 1);
  EXPECT_EQ(st.histCount, 0);  // a(v1), c(v3) are <= 3
  EXPECT_EQ(st.maxVer, 4u);
  EXPECT_NE(s.find("Tb|4"), std::string::npos);
}

TEST_F(LookupHistoryTest, FirstUploadIsKeyframe) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);
  LookupHistory::addWord(cachePath, "b", Status::Direct);
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  bool kf = false;
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_TRUE(kf);
  EXPECT_EQ(st.histCount, 2);
  EXPECT_TRUE(syncFileContents().empty());  // no commit yet -> watermark unwritten
}

TEST_F(LookupHistoryTest, CommitThenSubsequentUploadIsDelta) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);  // v1
  LookupHistory::addWord(cachePath, "b", Status::Direct);  // v2
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  bool kf = false;
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  ASSERT_TRUE(kf);
  LookupHistory::commitUpload(cachePath, st, kf);
  EXPECT_EQ(syncFileContents(), "2 0");  // lastVer=2, uploadsSinceKeyframe=0

  LookupHistory::addWord(cachePath, "c", Status::Direct);  // v3
  const size_t n = LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_FALSE(kf);
  EXPECT_EQ(st.histCount, 1);  // only c
  EXPECT_NE(blobStr(buf, n).find("c|D|3"), std::string::npos);
}

TEST_F(LookupHistoryTest, WatermarkAdvancesOnlyOnCommit) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);
  LookupHistory::addWord(cachePath, "b", Status::Direct);
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  bool kf = false;
  // No commit between the two calls: both stay keyframe (watermark never moved).
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_TRUE(kf);
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_TRUE(kf);
  // Commit -> now a delta.
  LookupHistory::commitUpload(cachePath, st, kf);
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_FALSE(kf);
}

TEST_F(LookupHistoryTest, KeyframeEveryNForcesPeriodicKeyframe) {
  LookupHistory::addWord(cachePath, "seed", Status::Direct);
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  bool kf = false;
  // Initial keyframe + commit (uploadsSinceKeyframe -> 0).
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  ASSERT_TRUE(kf);
  LookupHistory::commitUpload(cachePath, st, kf);

  // KEYFRAME_EVERY delta uploads stay deltas.
  for (uint32_t i = 0; i < LookupHistory::KEYFRAME_EVERY; i++) {
    char w[16];
    std::snprintf(w, sizeof(w), "w%u", i);
    LookupHistory::addWord(cachePath, w, Status::Direct);
    LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
    EXPECT_FALSE(kf) << "iteration " << i << " should be a delta";
    LookupHistory::commitUpload(cachePath, st, kf);
  }
  // The next upload is forced back to a keyframe.
  LookupHistory::addWord(cachePath, "last", Status::Direct);
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_TRUE(kf);
}

TEST_F(LookupHistoryTest, VersionRegressionForcesKeyframe) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);  // v1
  LookupHistory::addWord(cachePath, "b", Status::Direct);  // v2
  LookupHistory::addWord(cachePath, "c", Status::Direct);  // v3 -> clock=3
  // Watermark ahead of the clock (e.g. cache rebuilt, counter lost) -> keyframe.
  writeSyncFile("99 0");
  uint8_t buf[256];
  LookupHistory::BlobStats st;
  bool kf = false;
  LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_TRUE(kf);
  EXPECT_EQ(st.histCount, 3);  // full snapshot, not a (broken) delta
}

TEST_F(LookupHistoryTest, DeltaTruncationFallsBackToKeyframe) {
  LookupHistory::addWord(cachePath, "a", Status::Direct);  // v1
  LookupHistory::addWord(cachePath, "b", Status::Direct);  // v2
  uint8_t big[256];
  LookupHistory::BlobStats st;
  bool kf = false;
  LookupHistory::serializeForUpload(cachePath, big, sizeof(big), &st, &kf);
  ASSERT_TRUE(kf);
  LookupHistory::commitUpload(cachePath, st, kf);  // watermark -> v2

  // Two long entries form a delta that cannot fit a tiny cap -> truncation ->
  // serializeForUpload must fall back to a keyframe rather than silently drop a line.
  LookupHistory::addWord(cachePath, "longwordone", Status::Direct);  // v3
  LookupHistory::addWord(cachePath, "longwordtwo", Status::Direct);  // v4
  uint8_t small[20];
  LookupHistory::serializeForUpload(cachePath, small, sizeof(small), &st, &kf);
  EXPECT_TRUE(kf);
}

TEST_F(LookupHistoryTest, DeltaRoundTripPropagatesAddUpdateDelete) {
  const std::string pathB = cachePath + "_B";
  std::filesystem::create_directories(pathB);
  for (const char* suff :
       {"/dictionary_history.txt", "/dictionary_history.tomb", "/dictionary_history.ver", "/dictionary_history.sync"})
    std::remove((pathB + suff).c_str());

  uint8_t buf[1024];
  LookupHistory::BlobStats st;
  bool kf = false;

  // A adds alpha -> keyframe; merge into B.
  LookupHistory::addWord(cachePath, "alpha", Status::Direct);  // v1
  size_t n = LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_TRUE(kf);
  LookupHistory::mergeBlob(pathB, buf, n);
  LookupHistory::commitUpload(cachePath, st, kf);
  {
    const auto e = LookupHistory::load(pathB);
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0].word, "alpha");
  }

  // A adds beta -> delta carries just beta.
  LookupHistory::addWord(cachePath, "beta", Status::Stem);  // v2
  n = LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_FALSE(kf);
  EXPECT_EQ(st.histCount, 1);
  LookupHistory::mergeBlob(pathB, buf, n);
  LookupHistory::commitUpload(cachePath, st, kf);
  EXPECT_EQ(LookupHistory::load(pathB).size(), 2u);

  // A updates alpha (re-lookup) -> delta carries the higher version.
  LookupHistory::addWord(cachePath, "alpha", Status::Suggestion);  // v3
  n = LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_FALSE(kf);
  LookupHistory::mergeBlob(pathB, buf, n);
  LookupHistory::commitUpload(cachePath, st, kf);
  {
    const auto e = LookupHistory::load(pathB);
    bool found = false;
    for (const auto& x : e)
      if (x.word == "alpha") {
        EXPECT_EQ(x.status, Status::Suggestion);
        found = true;
      }
    EXPECT_TRUE(found);
  }

  // A deletes beta -> delta carries the tombstone -> B drops beta.
  EXPECT_TRUE(LookupHistory::removeAt(cachePath, 0));  // oldest=beta -> tombstone v4
  n = LookupHistory::serializeForUpload(cachePath, buf, sizeof(buf), &st, &kf);
  EXPECT_FALSE(kf);
  EXPECT_GE(st.tombCount, 1);
  LookupHistory::mergeBlob(pathB, buf, n);
  LookupHistory::commitUpload(cachePath, st, kf);
  {
    const auto e = LookupHistory::load(pathB);
    for (const auto& x : e) EXPECT_NE(x.word, "beta");
  }
}

// mergeBlob caches the per-word history/tombstone versions in a slot array indexed by blob
// line, instead of rescanning both files once per line. The cache is only correct if the
// slot a line gets while merging is the slot the file scan filled for it, so this pins the
// two words to versions that make a swap change the OUTCOME rather than just the reads:
// gamma is old (must accept the remote update), alpha is newer (must reject it). An off-by-
// one in either walk inverts both decisions.
TEST_F(LookupHistoryTest, MergeBlobResolvesVersionsPerLineNotPerSlotOrder) {
  LookupHistory::addWord(cachePath, "gamma", Status::Direct);                              // v1
  for (int i = 0; i < 9; i++) LookupHistory::addWord(cachePath, "alpha", Status::Direct);  // ends at v10

  // A short junk line between the two real ones: mergeBlob advances its slot for EVERY
  // line, matched or not, so a filler that both walks must count identically belongs here.
  const std::string blob = "Hgamma|S|5\n#\nHalpha|S|5\n";
  int deleted = -1;
  const int added =
      LookupHistory::mergeBlob(cachePath, reinterpret_cast<const uint8_t*>(blob.data()), blob.size(), &deleted);

  EXPECT_EQ(added, 1);  // gamma only
  EXPECT_EQ(deleted, 0);
  const auto e = LookupHistory::load(cachePath);
  bool sawGamma = false, sawAlpha = false;
  for (const auto& x : e) {
    if (x.word == "gamma") {
      sawGamma = true;
      EXPECT_EQ(x.status, Status::Suggestion);  // v5 > v1 -> remote wins
    }
    if (x.word == "alpha") {
      sawAlpha = true;
      EXPECT_EQ(x.status, Status::Direct);  // v5 <= v10 -> local wins
    }
  }
  EXPECT_TRUE(sawGamma);
  EXPECT_TRUE(sawAlpha);
}

// Every applied add/delete rewrites the history file, so the cached versions are dropped and
// rebuilt before the next lookup. This walks a blob where each line mutates, which is the
// path that would break if the cache were patched incrementally or never invalidated: the
// second and later lines would read pre-merge state.
TEST_F(LookupHistoryTest, MergeBlobAppliesEveryLineWhenEachOneMutates) {
  LookupHistory::addWord(cachePath, "keep", Status::Direct);  // v1

  const std::string blob = "Hone|D|5\nHtwo|D|6\nHthree|D|7\nTkeep|8\n";
  int deleted = -1;
  const int added =
      LookupHistory::mergeBlob(cachePath, reinterpret_cast<const uint8_t*>(blob.data()), blob.size(), &deleted);

  EXPECT_EQ(added, 3);
  EXPECT_EQ(deleted, 1);
  const auto e = LookupHistory::load(cachePath);
  EXPECT_EQ(e.size(), 3u);
  for (const auto& x : e) EXPECT_NE(x.word, "keep");

  // Re-merging the same blob must be a no-op: the versions now match exactly, which also
  // proves the rebuilt cache reflects the post-merge file rather than the snapshot.
  int deleted2 = -1;
  EXPECT_EQ(LookupHistory::mergeBlob(cachePath, reinterpret_cast<const uint8_t*>(blob.data()), blob.size(), &deleted2),
            0);
  EXPECT_EQ(deleted2, 0);
}

}  // namespace
