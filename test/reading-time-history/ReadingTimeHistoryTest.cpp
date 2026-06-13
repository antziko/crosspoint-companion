// Covers the heatmap intensity-level redesign: ReadingTimeHistory went from a
// 1-bit/day "did they read at all" presence bitmap to a 2-bit/day intensity
// level (None/Light/Moderate/Heavy) classified from an *accumulated* per-day
// seconds total (recordDay() fires once per reading session, so a day can
// receive several calls before it's finalized). These tests exercise exactly
// the paths that didn't exist -- and couldn't be wrong -- in the old
// "set the bit once" version.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
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

  record(h, 2024, 3, 11, 10 * 60);                        // 10 days later -> Light
  EXPECT_EQ(h.getHeatmapLevel(10), HeatmapLevel::Heavy);  // shifted intact
  EXPECT_EQ(h.getHeatmapLevel(0), HeatmapLevel::Light);
  // Untouched gap days remain None (never written, not just zeroed-but-set).
  EXPECT_EQ(h.getHeatmapLevel(5), HeatmapLevel::None);
  EXPECT_FALSE(h.isHeatmapDaySet(5));
}

TEST(ReadingTimeHistoryHeatmap, BackdatedSessionUpgradesButNeverDowngrades) {
  ReadingTimeHistory h;
  record(h, 2024, 3, 10, 5 * 60);  // anchor = Mar 10, Light
  record(h, 2024, 3, 12, 5 * 60);  // new anchor = Mar 12; Mar 10 now 2 slots back
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

// --- Cross-device merge (KOReader stats sync, mergeFrom) ----------------------

TEST(ReadingTimeHistoryMerge, WeeklyMonthlyYearlySumByDateKeyNewestFirst) {
  // Two devices reading the same + different periods. Mondays: 2024-02-26,
  // 2024-03-04, 2024-03-11 are all Mondays, so each lands in its own weekly bucket.
  ReadingTimeHistory a;
  record(a, 2024, 2, 26, 100);  // wk Feb26
  record(a, 2024, 3, 4, 600);   // wk Mar4   (shared)
  ReadingTimeHistory b;
  record(b, 2024, 3, 4, 300);   // wk Mar4   (shared)
  record(b, 2024, 3, 11, 200);  // wk Mar11

  a.mergeFrom(b);

  // Newest-first: Mar11 (b-only), Mar4 (summed 900), Feb26 (a-only).
  EXPECT_EQ(a.weekly[0].month, 3);
  EXPECT_EQ(a.weekly[0].day, 11);
  EXPECT_EQ(a.weekly[0].seconds, 200u);
  EXPECT_EQ(a.weekly[1].day, 4);
  EXPECT_EQ(a.weekly[1].seconds, 900u);  // 600 + 300 summed by key
  EXPECT_EQ(a.weekly[2].day, 26);
  EXPECT_EQ(a.weekly[2].seconds, 100u);
  EXPECT_EQ(a.weekly[3].seconds, 0u);  // nothing older

  // Monthly: a-only Feb (100). March = a's Mar4 (600) + b's Mar4+Mar11 (500) = 1100.
  EXPECT_EQ(a.monthly[0].year, 2024);
  EXPECT_EQ(a.monthly[0].month, 3);
  EXPECT_EQ(a.monthly[0].seconds, 1100u);
  EXPECT_EQ(a.monthly[1].month, 2);
  EXPECT_EQ(a.monthly[1].seconds, 100u);

  // Yearly: all 2024 -> 700 (a) + 500 (b) = 1200.
  EXPECT_EQ(a.yearly[0].year, 2024);
  EXPECT_EQ(a.yearly[0].seconds, 1200u);
  EXPECT_EQ(a.yearly[1].seconds, 0u);
}

TEST(ReadingTimeHistoryMerge, HeatmapTakesPerDayMaxLevelAcrossAnchors) {
  // Different anchors: a's newest day is Mar 10, b's is Mar 12.
  ReadingTimeHistory a;
  record(a, 2024, 3, 10, 5 * 60);  // Light, anchor Mar10
  ReadingTimeHistory b;
  record(b, 2024, 3, 12, 90 * 60);  // Heavy, anchor Mar12

  a.mergeFrom(b);

  // Merged anchor = Mar12. b's Heavy at slot 0; a's Light slid back to slot 2.
  EXPECT_EQ(a.getHeatmapLevel(0), HeatmapLevel::Heavy);
  EXPECT_EQ(a.getHeatmapLevel(2), HeatmapLevel::Light);
  EXPECT_EQ(a.getHeatmapLevel(1), HeatmapLevel::None);  // Mar11 untouched by either
}

TEST(ReadingTimeHistoryMerge, HeatmapSameDayPromotesUpButNeverDown) {
  // Same calendar day, both directions: max wins, lower never demotes higher.
  ReadingTimeHistory lightThenModerate;
  record(lightThenModerate, 2024, 3, 4, 5 * 60);  // Light
  ReadingTimeHistory moderate;
  record(moderate, 2024, 3, 4, 40 * 60);  // Moderate (same day)
  lightThenModerate.mergeFrom(moderate);
  EXPECT_EQ(lightThenModerate.getHeatmapLevel(0), HeatmapLevel::Moderate);

  ReadingTimeHistory moderateThenLight;
  record(moderateThenLight, 2024, 3, 4, 40 * 60);  // Moderate
  ReadingTimeHistory light;
  record(light, 2024, 3, 4, 5 * 60);  // Light (same day)
  moderateThenLight.mergeFrom(light);
  EXPECT_EQ(moderateThenLight.getHeatmapLevel(0), HeatmapLevel::Moderate);  // not demoted
}

TEST(ReadingTimeHistoryMerge, MergeIntoEmptyAdoptsOther) {
  ReadingTimeHistory empty;
  ReadingTimeHistory b;
  record(b, 2024, 3, 12, 90 * 60);
  empty.mergeFrom(b);
  EXPECT_EQ(empty.getHeatmapLevel(0), HeatmapLevel::Heavy);
  EXPECT_EQ(empty.yearly[0].seconds, 90u * 60u);
  EXPECT_EQ(empty.weekly[0].seconds, 90u * 60u);
}

TEST(ReadingTimeHistoryMerge, WeeklyRingOverflowKeepsNewest) {
  // Fill a with the 52 most recent weeks, b with one even newer week. After merge
  // the array still holds 52 entries and the newest is b's, oldest a-week dropped.
  ReadingTimeHistory a;
  // 52 consecutive Mondays starting 2023-01-02 (a Monday), going forward.
  // recordDay prepends each newer week; after 52 the array is full.
  uint16_t y = 2023;
  uint8_t mo = 1, d = 2;
  for (size_t i = 0; i < ReadingTimeHistory::WEEKLY_COUNT; i++) {
    record(a, y, mo, d, 60);
    // advance 7 days
    uint32_t idx = readingHistoryDayIndex(y, mo, d) + 7;
    readingHistoryDateFromDayIndex(idx, y, mo, d);
  }
  const uint32_t oldestKeyBefore = readingHistoryDayIndex(a.weekly[ReadingTimeHistory::WEEKLY_COUNT - 1].year,
                                                          a.weekly[ReadingTimeHistory::WEEKLY_COUNT - 1].month,
                                                          a.weekly[ReadingTimeHistory::WEEKLY_COUNT - 1].day);
  ReadingTimeHistory b;
  record(b, y, mo, d, 60);  // one week newer than a's newest

  a.mergeFrom(b);

  // Still exactly WEEKLY_COUNT active entries, newest is b's week, and the
  // previously-oldest week was pushed out.
  EXPECT_GT(a.weekly[ReadingTimeHistory::WEEKLY_COUNT - 1].seconds, 0u);
  const uint32_t newestKey = readingHistoryDayIndex(a.weekly[0].year, a.weekly[0].month, a.weekly[0].day);
  EXPECT_EQ(newestKey, readingHistoryDayIndex(y, mo, d));
  const uint32_t oldestKeyAfter = readingHistoryDayIndex(a.weekly[ReadingTimeHistory::WEEKLY_COUNT - 1].year,
                                                         a.weekly[ReadingTimeHistory::WEEKLY_COUNT - 1].month,
                                                         a.weekly[ReadingTimeHistory::WEEKLY_COUNT - 1].day);
  EXPECT_GT(oldestKeyAfter, oldestKeyBefore);  // oldest advanced -> one dropped
}

// --- Wire blob (de)serialization ---------------------------------------------

TEST(ReadingTimeHistoryBlob, RoundTripPreservesBucketsAndHeatmapLevels) {
  ReadingTimeHistory h;
  record(h, 2024, 3, 4, 40 * 60);    // Moderate
  record(h, 2024, 3, 5, 90 * 60);    // Heavy, new anchor
  record(h, 2023, 12, 25, 10 * 60);  // older year/month/week

  uint8_t buf[ReadingTimeHistory::BLOB_MAX_BYTES];
  const size_t n = h.serializeBlob(buf, sizeof(buf));
  ASSERT_EQ(n, ReadingTimeHistory::BLOB_MAX_BYTES);

  ReadingTimeHistory r;
  ASSERT_TRUE(r.deserializeBlob(buf, n));

  // Bucket arrays are byte-identical (POD), heatmap levels + anchor preserved.
  EXPECT_EQ(0, std::memcmp(r.weekly, h.weekly, sizeof(h.weekly)));
  EXPECT_EQ(0, std::memcmp(r.monthly, h.monthly, sizeof(h.monthly)));
  EXPECT_EQ(0, std::memcmp(r.yearly, h.yearly, sizeof(h.yearly)));
  EXPECT_EQ(r.heatmapAnchorDay, h.heatmapAnchorDay);
  EXPECT_EQ(r.getHeatmapLevel(0), HeatmapLevel::Heavy);
  EXPECT_EQ(r.getHeatmapLevel(1), HeatmapLevel::Moderate);
  // heatmapAnchorSeconds is intentionally not transmitted.
  EXPECT_EQ(r.heatmapAnchorSeconds, 0u);
}

TEST(ReadingTimeHistoryBlob, DeserializeRejectsShortOrBadVersion) {
  ReadingTimeHistory h;
  record(h, 2024, 3, 4, 40 * 60);
  uint8_t buf[ReadingTimeHistory::BLOB_MAX_BYTES];
  const size_t n = h.serializeBlob(buf, sizeof(buf));
  ASSERT_EQ(n, ReadingTimeHistory::BLOB_MAX_BYTES);

  ReadingTimeHistory r;
  EXPECT_FALSE(r.deserializeBlob(buf, n - 1));          // too short
  EXPECT_EQ(r.getHeatmapLevel(0), HeatmapLevel::None);  // left default-constructed

  uint8_t bad[ReadingTimeHistory::BLOB_MAX_BYTES];
  std::memcpy(bad, buf, n);
  bad[0] = 0xFF;  // wrong version byte
  EXPECT_FALSE(r.deserializeBlob(bad, n));
}

TEST(ReadingTimeHistoryBlob, SerializeFailsWhenBufferTooSmall) {
  ReadingTimeHistory h;
  uint8_t small[8];
  EXPECT_EQ(h.serializeBlob(small, sizeof(small)), 0u);
}

// A blob round-trip followed by a merge is the real sync path (other device's
// blob -> deserialize -> fold). Guards that the two compose correctly.
TEST(ReadingTimeHistoryBlob, RoundTripThenMergeMatchesDirectMerge) {
  ReadingTimeHistory local;
  record(local, 2024, 3, 4, 30 * 60);  // Moderate-ish
  record(local, 2024, 3, 5, 5 * 60);   // Light, anchor Mar5

  ReadingTimeHistory remote;
  record(remote, 2024, 3, 5, 90 * 60);  // Heavy same anchor day
  record(remote, 2024, 3, 6, 10 * 60);  // newer day Mar6

  // Path A: direct merge.
  ReadingTimeHistory direct = local;
  direct.mergeFrom(remote);

  // Path B: remote through the wire, then merge.
  uint8_t buf[ReadingTimeHistory::BLOB_MAX_BYTES];
  const size_t n = remote.serializeBlob(buf, sizeof(buf));
  ReadingTimeHistory remoteWire;
  ASSERT_TRUE(remoteWire.deserializeBlob(buf, n));
  ReadingTimeHistory viaWire = local;
  viaWire.mergeFrom(remoteWire);

  EXPECT_EQ(0, std::memcmp(direct.weekly, viaWire.weekly, sizeof(direct.weekly)));
  EXPECT_EQ(0, std::memcmp(direct.monthly, viaWire.monthly, sizeof(direct.monthly)));
  EXPECT_EQ(0, std::memcmp(direct.yearly, viaWire.yearly, sizeof(direct.yearly)));
  EXPECT_EQ(0, std::memcmp(direct.heatmapBits, viaWire.heatmapBits, sizeof(direct.heatmapBits)));
  EXPECT_EQ(direct.heatmapAnchorDay, viaWire.heatmapAnchorDay);
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
