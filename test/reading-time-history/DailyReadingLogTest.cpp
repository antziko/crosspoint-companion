#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "src/activities/reader/DailyReadingLog.h"

namespace {
std::string tempPath(const char* name) { return std::string(::testing::TempDir()) + name; }
}  // namespace

TEST(DailyReadingLog, AddsUpEachDayOfTheWeek) {
  const std::string path = tempPath("daily_week.bin");
  std::remove(path.c_str());
  ASSERT_TRUE(DailyReadingLog::add(path.c_str(), 9814, 600));
  ASSERT_TRUE(DailyReadingLog::add(path.c_str(), 9814, 300));
  ASSERT_TRUE(DailyReadingLog::add(path.c_str(), 9817, 1200));

  uint16_t week[7];
  ASSERT_TRUE(DailyReadingLog::readWeek(path.c_str(), 9813, week));
  EXPECT_EQ(week[0], 0);
  EXPECT_EQ(week[1], 900);
  EXPECT_EQ(week[4], 1200);
  EXPECT_EQ(week[5], 0);  // after the newest day
}

TEST(DailyReadingLog, ANewDayClearsTheSlotsItWrapsOnto) {
  const std::string path = tempPath("daily_wrap.bin");
  std::remove(path.c_str());
  ASSERT_TRUE(DailyReadingLog::add(path.c_str(), 1000, 500));
  // Same ring slot, one window later: the old day must not show through.
  ASSERT_TRUE(DailyReadingLog::add(path.c_str(), 1000 + DailyReadingLog::DAYS, 60));

  uint16_t week[7];
  ASSERT_TRUE(DailyReadingLog::readWeek(path.c_str(), 1000, week));
  EXPECT_EQ(week[0], 0);  // fell out of the window
  ASSERT_TRUE(DailyReadingLog::readWeek(path.c_str(), 1000 + DailyReadingLog::DAYS, week));
  EXPECT_EQ(week[0], 60);
}

TEST(DailyReadingLog, ADayOutsideTheWindowIsIgnoredAndSecondsSaturate) {
  const std::string path = tempPath("daily_old.bin");
  std::remove(path.c_str());
  ASSERT_TRUE(DailyReadingLog::add(path.c_str(), 5000, 70000));
  ASSERT_TRUE(DailyReadingLog::add(path.c_str(), 5000 - DailyReadingLog::DAYS, 100));

  uint16_t week[7];
  ASSERT_TRUE(DailyReadingLog::readWeek(path.c_str(), 5000, week));
  EXPECT_EQ(week[0], DailyReadingLog::MAX_SECONDS);
  ASSERT_TRUE(DailyReadingLog::readWeek(path.c_str(), 5000 - DailyReadingLog::DAYS, week));
  EXPECT_EQ(week[0], 0);
}

TEST(DailyReadingLog, NoFileReadsAsEmpty) {
  uint16_t week[7] = {1, 1, 1, 1, 1, 1, 1};
  EXPECT_FALSE(DailyReadingLog::readWeek(tempPath("daily_none.bin").c_str(), 100, week));
  EXPECT_EQ(week[3], 0);
}
