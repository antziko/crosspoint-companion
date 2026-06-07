// Covers the heatmap intensity-level redesign: ReadingTimeHistory went from a
// 1-bit/day "did they read at all" presence bitmap to a 2-bit/day intensity
// level (None/Light/Moderate/Heavy) classified from an *accumulated* per-day
// seconds total (recordDay() fires once per reading session, so a day can
// receive several calls before it's finalized). These tests exercise exactly
// the paths that didn't exist -- and couldn't be wrong -- in the old
// "set the bit once" version.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "src/activities/reader/ReadingTimeHistory.h"

namespace {

using HeatmapLevel = ReadingTimeHistory::HeatmapLevel;

constexpr uint32_t kLightMax = ReadingTimeHistory::HEATMAP_LIGHT_MAX_SECONDS;
constexpr uint32_t kModerateMax = ReadingTimeHistory::HEATMAP_MODERATE_MAX_SECONDS;

uint8_t dowFor(uint16_t year, uint8_t month, uint8_t day) {
  return readingHistoryDayOfWeek(readingHistoryDayIndex(year, month, day));
}

// recordDay's dayOfWeek param only feeds the weekly bucket, not the heatmap --
// computing it for real keeps callers honest without coupling heatmap
// assertions to weekly-bucket behavior.
void record(ReadingTimeHistory& h, uint16_t year, uint8_t month, uint8_t day, uint32_t seconds) {
  h.recordDay(year, month, day, dowFor(year, month, day), seconds);
}

}  // namespace

TEST(ReadingTimeHistoryHeatmap, ThresholdBoundariesFromASingleSession) {
  struct Case {
    uint32_t seconds;
    HeatmapLevel expected;
  };
  // Fresh history per case: the running total equals the single session's
  // length exactly, so this pins classifyHeatmapLevel's boundary inclusivity.
  const Case cases[] = {
      {0, HeatmapLevel::None},  // recordDay no-ops on 0s -- nothing recorded
      {1, HeatmapLevel::Light},
      {kLightMax, HeatmapLevel::Light},
      {kLightMax + 1, HeatmapLevel::Moderate},
      {kModerateMax, HeatmapLevel::Moderate},
      {kModerateMax + 1, HeatmapLevel::Heavy},
  };
  for (const auto& c : cases) {
    ReadingTimeHistory h;
    record(h, 2024, 3, 4, c.seconds);
    EXPECT_EQ(h.getHeatmapLevel(0), c.expected) << "seconds=" << c.seconds;
  }
}

TEST(ReadingTimeHistoryHeatmap, SameDayAccumulationCrossesABoundary) {
  ReadingTimeHistory h;
  record(h, 2024, 3, 4, 20 * 60);
  EXPECT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Light);

  // Second session same day: 20+20=40min combined total -> Moderate. The old
  // "set bit once per call" model could never represent this -- each call only
  // ever saw its own 20-minute slice.
  record(h, 2024, 3, 4, 20 * 60);
  EXPECT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Moderate);
  EXPECT_EQ(h.heatmapAnchorSeconds, 40u * 60u);
}

TEST(ReadingTimeHistoryHeatmap, DayRolloverFinalizesLocksAndResetsRunningTotal) {
  ReadingTimeHistory h;
  record(h, 2024, 3, 4, 20 * 60);
  record(h, 2024, 3, 4, 20 * 60);  // day 1 total: 40min -> Moderate
  ASSERT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Moderate);

  record(h, 2024, 3, 5, 10 * 60);  // next day, fresh running total
  // Previous day is locked at its finalized level, now one slot back.
  EXPECT_EQ(h.getHeatmapLevel(1), HeatmapLevel::Moderate);
  // New day reflects only its own total, not a continuation of the old one.
  EXPECT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Light);
  EXPECT_EQ(h.heatmapAnchorSeconds, 10u * 60u);

  // A second session on the new day must combine with *this* day's total
  // (10+25=35min -> Moderate), proving the running total reset rather than
  // carrying over the old day's 40 minutes (which would also read Moderate,
  // but for the wrong reason -- 10+40=50min is still <= 1h).
  record(h, 2024, 3, 5, 25 * 60);
  EXPECT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Moderate);
  EXPECT_EQ(h.heatmapAnchorSeconds, 35u * 60u);
  EXPECT_EQ(h.getHeatmapLevel(1), HeatmapLevel::Moderate);  // still locked
}

TEST(ReadingTimeHistoryHeatmap, MultiDayShiftCrossesPackedByteBoundaries) {
  ReadingTimeHistory h;
  // 2 bits/day -> 4 days/byte. A 10-day jump must shift across multiple byte
  // boundaries in heatmapBits; this exercises heatmapShiftOlder's temp-buffer
  // copy at that granularity (not just within a single byte).
  record(h, 2024, 3, 1, 90 * 60);  // > 1h -> Heavy
  ASSERT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Heavy);

  record(h, 2024, 3, 11, 10 * 60);  // 10 days later -> Light
  EXPECT_EQ(h.getHeatmapLevel(10), HeatmapLevel::Heavy);  // shifted intact
  EXPECT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Light);
  // Untouched gap days remain None (never written, not just zeroed-but-set).
  EXPECT_EQ(h.getHeatmapLevel(5), HeatmapLevel::None);
  EXPECT_FALSE(h.isHeatmapDaySet(5));
}

TEST(ReadingTimeHistoryHeatmap, BackdatedSessionUpgradesButNeverDowngrades) {
  ReadingTimeHistory h;
  record(h, 2024, 3, 10, 5 * 60);   // anchor = Mar 10, Light
  record(h, 2024, 3, 12, 5 * 60);   // new anchor = Mar 12; Mar 10 now 2 slots back
  ASSERT_EQ(h.getHeatmapLevel(2), HeatmapLevel::Light);

  // Backdated session lands on the already-finalized Mar 10 with a longer
  // duration -> single-session classification (Heavy) raises that slot.
  record(h, 2024, 3, 10, 90 * 60);
  EXPECT_EQ(h.getHeatmapLevel(2), HeatmapLevel::Heavy);

  // A further backdated session that day with a *shorter* duration must not
  // pull the slot back down -- there's no stored running total for past days,
  // so upgrade-only is the documented, deliberate tradeoff (recordDay's
  // "Backdated session" branch comment).
  record(h, 2024, 3, 10, 5 * 60);
  EXPECT_EQ(h.getHeatmapLevel(2), HeatmapLevel::Heavy);
}

TEST(ReadingTimeHistoryPersistence, RoundTripPreservesRunningTotalAcrossReload) {
  const std::string path = ::testing::TempDir() + "reading_time_history_test.bin";
  std::remove(path.c_str());

  {
    ReadingTimeHistory h;
    record(h, 2024, 3, 4, 20 * 60);  // 20min -> Light
    ASSERT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Light);
    ASSERT_TRUE(ReadingTimeHistory::save(path, h));
  }

  ReadingTimeHistory loaded;
  ASSERT_TRUE(ReadingTimeHistory::load(path, loaded));
  EXPECT_EQ(loaded.getHeatmapLevel(0), HeatmapLevel::Light);
  EXPECT_EQ(loaded.heatmapAnchorSeconds, 20u * 60u);

  // Simulates a restart mid-day: the next same-day session must combine with
  // the *persisted* running total (20+20=40min -> Moderate). If
  // heatmapAnchorSeconds weren't serialized, this would read back as 0 and the
  // day would misclassify as Light from the second session's 20 minutes alone.
  record(loaded, 2024, 3, 4, 20 * 60);
  EXPECT_EQ(loaded.getHeatmapLevel(0), HeatmapLevel::Moderate);

  std::remove(path.c_str());
}

TEST(ReadingTimeHistoryHeatmap, PresenceWrapperAgreesWithLevelAcrossStates) {
  ReadingTimeHistory h;
  EXPECT_FALSE(h.hasAnyData());
  EXPECT_FALSE(h.isHeatmapDaySet(0));

  record(h, 2024, 3, 4, 5 * 60);   // Light
  record(h, 2024, 3, 5, 90 * 60);  // Heavy, new anchor; Mar 4 now 1 slot back

  EXPECT_TRUE(h.hasAnyData());
  EXPECT_EQ(h.isHeatmapDaySet(0), h.getHeatmapLevel(0) != HeatmapLevel::None);
  EXPECT_EQ(h.isHeatmapDaySet(1), h.getHeatmapLevel(1) != HeatmapLevel::None);
  EXPECT_TRUE(h.isHeatmapDaySet(0));
  EXPECT_TRUE(h.isHeatmapDaySet(1));

  // A day that was never recorded reads as both "no level" and "not set" --
  // the collapsed blank-cell semantics (no data == no activity, by design).
  EXPECT_EQ(h.getHeatmapLevel(500), HeatmapLevel::None);
  EXPECT_FALSE(h.isHeatmapDaySet(500));
}
