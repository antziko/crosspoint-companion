#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "BookReadingStats.h"

namespace {

void putLe16(std::vector<uint8_t>& d, uint16_t v) {
  d.push_back(v & 0xFF);
  d.push_back((v >> 8) & 0xFF);
}

void putLe32(std::vector<uint8_t>& d, uint32_t v) {
  d.push_back(v & 0xFF);
  d.push_back((v >> 8) & 0xFF);
  d.push_back((v >> 16) & 0xFF);
  d.push_back((v >> 24) & 0xFF);
}

// Builds a v3 (19-byte) stats.bin image: the on-disk format every device has
// before the remote-sync fields were added in v4.
std::vector<uint8_t> makeV3(uint32_t total, uint32_t unattributed, uint16_t pace, uint16_t samples, uint32_t lastDay,
                            uint8_t lastHour, uint8_t lastMinute) {
  std::vector<uint8_t> d;
  d.reserve(19);
  d.push_back(3);
  putLe32(d, total);
  putLe32(d, unattributed);
  putLe16(d, pace);
  putLe16(d, samples);
  putLe32(d, lastDay);
  d.push_back(lastHour);
  d.push_back(lastMinute);
  return d;
}

std::vector<uint8_t> makeV4(uint32_t total, uint32_t unattributed, uint16_t pace, uint16_t samples, uint32_t lastDay,
                            uint8_t lastHour, uint8_t lastMinute, uint32_t remoteSeconds, uint32_t remoteDay,
                            uint8_t remoteHour, uint8_t remoteMinute) {
  std::vector<uint8_t> d = makeV3(total, unattributed, pace, samples, lastDay, lastHour, lastMinute);
  d[0] = 4;
  d.reserve(29);
  putLe32(d, remoteSeconds);
  putLe32(d, remoteDay);
  d.push_back(remoteHour);
  d.push_back(remoteMinute);
  return d;
}

// Builds a v5 (33-byte) image: v4 layout plus the lastSyncReadingSeconds field.
std::vector<uint8_t> makeV5(uint32_t total, uint32_t unattributed, uint16_t pace, uint16_t samples, uint32_t lastDay,
                            uint8_t lastHour, uint8_t lastMinute, uint32_t remoteSeconds, uint32_t remoteDay,
                            uint8_t remoteHour, uint8_t remoteMinute, uint32_t lastSync) {
  std::vector<uint8_t> d = makeV4(total, unattributed, pace, samples, lastDay, lastHour, lastMinute, remoteSeconds,
                                  remoteDay, remoteHour, remoteMinute);
  d[0] = 5;
  d.reserve(33);
  putLe32(d, lastSync);
  return d;
}

// Builds a v6 (37-byte) image: v5 layout plus the lastSyncPromptSkipSeconds field.
std::vector<uint8_t> makeV6(uint32_t total, uint32_t unattributed, uint16_t pace, uint16_t samples, uint32_t lastDay,
                            uint8_t lastHour, uint8_t lastMinute, uint32_t remoteSeconds, uint32_t remoteDay,
                            uint8_t remoteHour, uint8_t remoteMinute, uint32_t lastSync, uint32_t lastSkip) {
  std::vector<uint8_t> d = makeV5(total, unattributed, pace, samples, lastDay, lastHour, lastMinute, remoteSeconds,
                                  remoteDay, remoteHour, remoteMinute, lastSync);
  d[0] = 6;
  d.reserve(37);
  putLe32(d, lastSkip);
  return d;
}

}  // namespace

TEST(BookReadingStatsParse, V3MigratesLosslesslyWithRemoteFieldsZeroed) {
  const auto img = makeV3(12345, 678, 42, 99, 9650, 21, 15);
  BookReadingStats s;
  ASSERT_TRUE(BookReadingStats::parse(img.data(), img.size(), s));
  EXPECT_EQ(s.totalReadingSeconds, 12345u);
  EXPECT_EQ(s.unattributedSeconds, 678u);
  EXPECT_EQ(s.avgSecondsPerForwardPage, 42u);
  EXPECT_EQ(s.paceSampleCount, 99u);
  EXPECT_EQ(s.lastReadDayIndex, 9650u);
  EXPECT_EQ(s.lastReadHour, 21u);
  EXPECT_EQ(s.lastReadMinute, 15u);
  EXPECT_EQ(s.remoteOtherSeconds, 0u);
  EXPECT_EQ(s.remoteLastReadDayIndex, 0u);
  EXPECT_EQ(s.remoteLastReadHour, 0u);
  EXPECT_EQ(s.remoteLastReadMinute, 0u);
  EXPECT_EQ(s.displayTotalSeconds(), 12345u);
}

TEST(BookReadingStatsParse, V3MigrationDoesNotLeakPriorRemoteValues) {
  // Reused struct (e.g. activity member) must not keep remote values from a
  // previously parsed v4 image when a v3 image is parsed into it.
  const auto v4 = makeV4(100, 0, 0, 0, 0, 0, 0, 555, 9000, 8, 30);
  const auto v3 = makeV3(200, 0, 0, 0, 0, 0, 0);
  BookReadingStats s;
  ASSERT_TRUE(BookReadingStats::parse(v4.data(), v4.size(), s));
  ASSERT_EQ(s.remoteOtherSeconds, 555u);
  ASSERT_TRUE(BookReadingStats::parse(v3.data(), v3.size(), s));
  EXPECT_EQ(s.totalReadingSeconds, 200u);
  EXPECT_EQ(s.remoteOtherSeconds, 0u);
  EXPECT_EQ(s.remoteLastReadDayIndex, 0u);
}

TEST(BookReadingStatsParse, V4RoundTrip) {
  const auto img = makeV4(300, 10, 7, 3, 9651, 6, 45, 240, 9650, 22, 5);
  BookReadingStats s;
  ASSERT_TRUE(BookReadingStats::parse(img.data(), img.size(), s));
  EXPECT_EQ(s.totalReadingSeconds, 300u);
  EXPECT_EQ(s.remoteOtherSeconds, 240u);
  EXPECT_EQ(s.remoteLastReadDayIndex, 9650u);
  EXPECT_EQ(s.remoteLastReadHour, 22u);
  EXPECT_EQ(s.remoteLastReadMinute, 5u);
  EXPECT_EQ(s.displayTotalSeconds(), 540u);  // 5 min local + 4 min remote = 9 min
  EXPECT_EQ(s.lastSyncReadingSeconds, 0u);   // absent in v4 → zeroed on upgrade
}

TEST(BookReadingStatsParse, V5RoundTrip) {
  const auto img = makeV5(300, 10, 7, 3, 9651, 6, 45, 240, 9650, 22, 5, 250);
  BookReadingStats s;
  ASSERT_TRUE(BookReadingStats::parse(img.data(), img.size(), s));
  EXPECT_EQ(s.totalReadingSeconds, 300u);
  EXPECT_EQ(s.remoteOtherSeconds, 240u);
  EXPECT_EQ(s.lastSyncReadingSeconds, 250u);
  EXPECT_EQ(s.lastSyncPromptSkipSeconds, 0u);  // absent in v5 → zeroed on upgrade
}

TEST(BookReadingStatsParse, V6RoundTrip) {
  const auto img = makeV6(300, 10, 7, 3, 9651, 6, 45, 240, 9650, 22, 5, 250, 280);
  BookReadingStats s;
  ASSERT_TRUE(BookReadingStats::parse(img.data(), img.size(), s));
  EXPECT_EQ(s.totalReadingSeconds, 300u);
  EXPECT_EQ(s.remoteOtherSeconds, 240u);
  EXPECT_EQ(s.lastSyncReadingSeconds, 250u);
  EXPECT_EQ(s.lastSyncPromptSkipSeconds, 280u);
}

TEST(BookReadingStatsParse, V5ToV6MigrationDoesNotLeakPriorSkipMarker) {
  // A reused struct must not keep lastSyncPromptSkipSeconds from a previously parsed
  // v6 image when a v5 image (no skip field) is parsed into it.
  const auto v6 = makeV6(100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 888);
  const auto v5 = makeV5(200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  BookReadingStats s;
  ASSERT_TRUE(BookReadingStats::parse(v6.data(), v6.size(), s));
  ASSERT_EQ(s.lastSyncPromptSkipSeconds, 888u);
  ASSERT_TRUE(BookReadingStats::parse(v5.data(), v5.size(), s));
  EXPECT_EQ(s.totalReadingSeconds, 200u);
  EXPECT_EQ(s.lastSyncPromptSkipSeconds, 0u);
}

TEST(BookReadingStatsParse, V4ToV5MigrationDoesNotLeakPriorSyncMarker) {
  // A reused struct must not keep lastSyncReadingSeconds from a previously parsed
  // v5 image when a v4 image (no marker field) is parsed into it.
  const auto v5 = makeV5(100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 777);
  const auto v4 = makeV4(200, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  BookReadingStats s;
  ASSERT_TRUE(BookReadingStats::parse(v5.data(), v5.size(), s));
  ASSERT_EQ(s.lastSyncReadingSeconds, 777u);
  ASSERT_TRUE(BookReadingStats::parse(v4.data(), v4.size(), s));
  EXPECT_EQ(s.totalReadingSeconds, 200u);
  EXPECT_EQ(s.lastSyncReadingSeconds, 0u);
}

TEST(BookReadingStatsParse, RejectsUnknownVersionsAndBadSizes) {
  BookReadingStats s;
  // v2 and below: rejected.
  auto v2 = makeV3(1, 2, 3, 4, 5, 6, 7);
  v2[0] = 2;
  EXPECT_FALSE(BookReadingStats::parse(v2.data(), v2.size(), s));
  // v3 with truncated payload: rejected.
  auto shortV3 = makeV3(1, 2, 3, 4, 5, 6, 7);
  shortV3.pop_back();
  EXPECT_FALSE(BookReadingStats::parse(shortV3.data(), shortV3.size(), s));
  // v4 with v3 length: rejected.
  auto v4short = makeV3(1, 2, 3, 4, 5, 6, 7);
  v4short[0] = 4;
  EXPECT_FALSE(BookReadingStats::parse(v4short.data(), v4short.size(), s));
  // Empty / null.
  EXPECT_FALSE(BookReadingStats::parse(nullptr, 29, s));
  EXPECT_FALSE(BookReadingStats::parse(v2.data(), 0, s));
}

TEST(BookReadingStatsIo, SaveLoadRoundTripThroughFile) {
  // The HalStorage host stub maps paths directly to the local filesystem.
  const std::string dir = ::testing::TempDir() + "stats_rt";
  std::remove((dir + "/stats.bin").c_str());
#ifdef _WIN32
  _mkdir(dir.c_str());
#else
  mkdir(dir.c_str(), 0755);
#endif

  BookReadingStats s;
  s.totalReadingSeconds = 300;
  s.unattributedSeconds = 60;
  s.avgSecondsPerForwardPage = 40;
  s.paceSampleCount = 12;
  s.lastReadDayIndex = 9651;
  s.lastReadHour = 7;
  s.lastReadMinute = 30;
  s.remoteOtherSeconds = 240;
  s.remoteLastReadDayIndex = 9650;
  s.remoteLastReadHour = 23;
  s.remoteLastReadMinute = 59;
  s.lastSyncReadingSeconds = 180;
  s.lastSyncPromptSkipSeconds = 210;
  s.save(dir);

  const BookReadingStats r = BookReadingStats::load(dir);
  EXPECT_EQ(r.totalReadingSeconds, 300u);
  EXPECT_EQ(r.unattributedSeconds, 60u);
  EXPECT_EQ(r.avgSecondsPerForwardPage, 40u);
  EXPECT_EQ(r.paceSampleCount, 12u);
  EXPECT_EQ(r.lastReadDayIndex, 9651u);
  EXPECT_EQ(r.remoteOtherSeconds, 240u);
  EXPECT_EQ(r.remoteLastReadDayIndex, 9650u);
  EXPECT_EQ(r.remoteLastReadHour, 23u);
  EXPECT_EQ(r.remoteLastReadMinute, 59u);
  EXPECT_EQ(r.lastSyncReadingSeconds, 180u);
  EXPECT_EQ(r.lastSyncPromptSkipSeconds, 210u);
  EXPECT_EQ(r.displayTotalSeconds(), 540u);
}

TEST(BookReadingStatsPace, FirstSampleSeedsAverage) {
  BookReadingStats s;
  s.recordForwardPageRead(30);
  EXPECT_EQ(s.avgSecondsPerForwardPage, 30u);
  EXPECT_EQ(s.paceSampleCount, 1u);
}

TEST(BookReadingStatsPace, BlendsTowardNewSamples) {
  BookReadingStats s;
  s.recordForwardPageRead(30);  // avg 30, count 1
  s.recordForwardPageRead(30);  // (30*1+30)/2 = 30, count 2
  EXPECT_EQ(s.avgSecondsPerForwardPage, 30u);
  s.recordForwardPageRead(60);  // (30*2+60)/3 = 40, count 3
  EXPECT_EQ(s.avgSecondsPerForwardPage, 40u);
  EXPECT_EQ(s.paceSampleCount, 3u);
}

TEST(BookReadingStatsPace, ReseedsStuckLowAverage) {
  // Simulate the old-trap state: average locked implausibly low with a heavy sample count.
  BookReadingStats s;
  s.avgSecondsPerForwardPage = 2;
  s.paceSampleCount = 300;
  // A real reading page far exceeds the stuck average -> restart from this sample, not blend.
  s.recordForwardPageRead(30);
  EXPECT_EQ(s.avgSecondsPerForwardPage, 30u);
  EXPECT_EQ(s.paceSampleCount, 1u);
}

TEST(BookReadingStatsPace, DoesNotReseedPlausibleAverage) {
  // A fast-but-plausible reader (avg >= 8s): a slower page must blend, never reseed.
  BookReadingStats s;
  s.avgSecondsPerForwardPage = 10;
  s.paceSampleCount = 10;
  s.recordForwardPageRead(60);  // (10*10+60)/11 = 14, blended (no reseed)
  EXPECT_EQ(s.avgSecondsPerForwardPage, 14u);
  EXPECT_EQ(s.paceSampleCount, 11u);
}

TEST(BookReadingStatsPace, DoesNotReseedSmallIncreaseOnLowAverage) {
  // Low average but the new sample is within 4x -> normal blend, no reseed.
  BookReadingStats s;
  s.avgSecondsPerForwardPage = 3;
  s.paceSampleCount = 5;
  s.recordForwardPageRead(10);  // 10 <= 4*3=12 -> blend: (3*5+10)/6 = 4
  EXPECT_EQ(s.avgSecondsPerForwardPage, 4u);
  EXPECT_EQ(s.paceSampleCount, 6u);
}
