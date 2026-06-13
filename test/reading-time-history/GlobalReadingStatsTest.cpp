#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "GlobalReadingStats.h"

namespace {

// Writes a raw v1 global_stats.bin (version byte + two u32 pods) — the on-disk
// format every device has before remoteOtherSeconds was added in v2.
void writeV1File(const std::string& path, uint32_t total, uint32_t unattributed) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const uint8_t version = 1;
  std::fwrite(&version, 1, 1, f);
  std::fwrite(&total, sizeof(total), 1, f);
  std::fwrite(&unattributed, sizeof(unattributed), 1, f);
  std::fclose(f);
}

}  // namespace

TEST(GlobalReadingStatsMigration, V1UpgradesLosslesslyWithRemoteZeroed) {
  const std::string dir = ::testing::TempDir();
  const std::string statsPath = dir + "gstats_v1.bin";
  const std::string historyPath = dir + "gstats_v1_history.bin";
  std::remove(historyPath.c_str());
  writeV1File(statsPath, 7200, 300);

  auto stats = std::make_unique<GlobalReadingStats>();
  EXPECT_TRUE(GlobalReadingStats::load(*stats, statsPath.c_str(), historyPath.c_str()));
  EXPECT_EQ(stats->totalReadingSeconds, 7200u);
  EXPECT_EQ(stats->unattributedSeconds, 300u);
  EXPECT_EQ(stats->remoteOtherSeconds, 0u);
  EXPECT_EQ(stats->displayTotalSeconds(), 7200u);
}

TEST(GlobalReadingStatsMigration, V2RoundTrip) {
  const std::string dir = ::testing::TempDir();
  const std::string statsPath = dir + "gstats_v2.bin";
  const std::string historyPath = dir + "gstats_v2_history.bin";

  auto stats = std::make_unique<GlobalReadingStats>();
  stats->totalReadingSeconds = 300;
  stats->unattributedSeconds = 60;
  stats->remoteOtherSeconds = 240;
  stats->save(statsPath.c_str(), historyPath.c_str());

  auto loaded = std::make_unique<GlobalReadingStats>();
  EXPECT_TRUE(GlobalReadingStats::load(*loaded, statsPath.c_str(), historyPath.c_str()));
  EXPECT_EQ(loaded->totalReadingSeconds, 300u);
  EXPECT_EQ(loaded->unattributedSeconds, 60u);
  EXPECT_EQ(loaded->remoteOtherSeconds, 240u);
  EXPECT_EQ(loaded->displayTotalSeconds(), 540u);  // 5 min local + 4 min remote = 9 min
}

TEST(GlobalReadingStatsRemote, RemoteHistoryRoundTripAndDisplayMerge) {
  const std::string dir = ::testing::TempDir();
  const std::string statsPath = dir + "g_rh.bin";
  const std::string historyPath = dir + "g_rh_history.bin";
  const std::string remotePath = dir + "g_rh_remote.bin";
  std::remove(historyPath.c_str());
  std::remove(remotePath.c_str());

  auto dow = [](uint16_t y, uint8_t m, uint8_t d) { return readingHistoryDayOfWeek(readingHistoryDayIndex(y, m, d)); };

  auto stats = std::make_unique<GlobalReadingStats>();
  stats->totalReadingSeconds = 600;
  stats->history.recordDay(2024, 3, 4, dow(2024, 3, 4), 600);          // local, wk Mar4
  stats->remoteHistory.recordDay(2024, 3, 4, dow(2024, 3, 4), 300);    // remote, wk Mar4 (shared)
  stats->remoteHistory.recordDay(2024, 3, 11, dow(2024, 3, 11), 200);  // remote, wk Mar11 (newer)
  stats->save(statsPath.c_str(), historyPath.c_str(), remotePath.c_str());

  auto loaded = std::make_unique<GlobalReadingStats>();
  ASSERT_TRUE(GlobalReadingStats::load(*loaded, statsPath.c_str(), historyPath.c_str(), remotePath.c_str()));
  // Remote snapshot persisted to its own file, independent of local history.
  EXPECT_EQ(loaded->remoteHistory.weekly[0].seconds, 200u);  // Mar11
  EXPECT_EQ(loaded->history.weekly[0].seconds, 600u);        // local untouched

  // displayHistory folds local + remote without mutating either source.
  auto disp = std::make_unique<ReadingTimeHistory>();
  loaded->displayHistory(*disp);
  EXPECT_EQ(disp->weekly[0].seconds, 200u);            // Mar11 (remote-only), newest
  EXPECT_EQ(disp->weekly[1].seconds, 900u);            // Mar4  (600 local + 300 remote)
  EXPECT_EQ(loaded->history.weekly[0].seconds, 600u);  // source unchanged by fold

  std::remove(statsPath.c_str());
  std::remove(historyPath.c_str());
  std::remove(remotePath.c_str());
}

TEST(GlobalReadingStatsRemote, AbsentRemoteFileLeavesSnapshotEmpty) {
  const std::string dir = ::testing::TempDir();
  const std::string statsPath = dir + "g_nrh.bin";
  const std::string historyPath = dir + "g_nrh_history.bin";
  const std::string remotePath = dir + "g_nrh_remote.bin";
  std::remove(remotePath.c_str());

  auto stats = std::make_unique<GlobalReadingStats>();
  stats->totalReadingSeconds = 120;
  stats->history.recordDay(2024, 3, 4, readingHistoryDayOfWeek(readingHistoryDayIndex(2024, 3, 4)), 120);
  // Save WITHOUT a remote path -> remote file never written.
  stats->save(statsPath.c_str(), historyPath.c_str());

  auto loaded = std::make_unique<GlobalReadingStats>();
  ASSERT_TRUE(GlobalReadingStats::load(*loaded, statsPath.c_str(), historyPath.c_str(), remotePath.c_str()));
  EXPECT_FALSE(loaded->remoteHistory.hasAnyData());

  // With no remote snapshot, the display history equals the local history.
  auto disp = std::make_unique<ReadingTimeHistory>();
  loaded->displayHistory(*disp);
  EXPECT_EQ(disp->weekly[0].seconds, loaded->history.weekly[0].seconds);

  std::remove(statsPath.c_str());
  std::remove(historyPath.c_str());
}

TEST(GlobalReadingStatsMigration, UnknownVersionStartsFresh) {
  const std::string dir = ::testing::TempDir();
  const std::string statsPath = dir + "gstats_v9.bin";
  const std::string historyPath = dir + "gstats_v9_history.bin";
  std::FILE* f = std::fopen(statsPath.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const uint8_t version = 9;
  std::fwrite(&version, 1, 1, f);
  std::fclose(f);

  auto stats = std::make_unique<GlobalReadingStats>();
  stats->totalReadingSeconds = 999;  // must be reset by load()
  EXPECT_FALSE(GlobalReadingStats::load(*stats, statsPath.c_str(), historyPath.c_str()));
  EXPECT_EQ(stats->totalReadingSeconds, 0u);
  EXPECT_EQ(stats->remoteOtherSeconds, 0u);
}
