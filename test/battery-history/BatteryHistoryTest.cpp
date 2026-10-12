#include <gtest/gtest.h>

#include <cstring>

#include "BatteryHistory.h"

using namespace battery;

namespace {
constexpr uint32_t T0 = 1790000000;  // 2026-09

Record rec(uint32_t seq, uint16_t boot, uint32_t uptime, uint8_t soc, uint32_t epoch = 0, uint8_t flags = 0) {
  Record r;
  r.seq = seq;
  r.boot = boot;
  r.uptimeS = uptime;
  r.soc = soc;
  r.epoch = epoch;
  r.flags = flags;
  return r;
}
}  // namespace

TEST(BatteryRecord, RoundTripsAndRejectsCorruption) {
  Record r = rec(42, 7, 1234, 88, T0, FLAG_USB | FLAG_WIFI);
  r.millivolts = 4012;
  r.event = Event::Unplug;
  uint8_t buf[RECORD_BYTES];
  encodeRecord(r, buf);

  Record out;
  ASSERT_TRUE(decodeRecord(buf, out));
  EXPECT_EQ(out.seq, 42u);
  EXPECT_EQ(out.boot, 7u);
  EXPECT_EQ(out.uptimeS, 1234u);
  EXPECT_EQ(out.soc, 88u);
  EXPECT_EQ(out.epoch, T0);
  EXPECT_EQ(out.millivolts, 4012u);
  EXPECT_EQ(out.event, Event::Unplug);
  EXPECT_EQ(out.flags, FLAG_USB | FLAG_WIFI);

  buf[5] ^= 0x10;  // torn write
  EXPECT_FALSE(decodeRecord(buf, out));
}

TEST(BatteryRecord, ZeroedSlotIsEmpty) {
  uint8_t buf[RECORD_BYTES] = {};
  Record out;
  EXPECT_FALSE(decodeRecord(buf, out));
}

TEST(BatteryHeader, PicksNewerValidSlot) {
  Header a;
  a.nextSeq = 10;
  Header b;
  b.nextSeq = 11;
  b.chargedPct = 250;
  uint8_t sa[HEADER_BYTES];
  uint8_t sb[HEADER_BYTES];
  encodeHeader(a, sa);
  encodeHeader(b, sb);

  Header out;
  ASSERT_TRUE(pickHeader(sa, sb, out));
  EXPECT_EQ(out.nextSeq, 11u);
  EXPECT_EQ(out.chargedPct, 250u);

  sb[8] ^= 1;  // newer slot torn: fall back to the older one
  ASSERT_TRUE(pickHeader(sa, sb, out));
  EXPECT_EQ(out.nextSeq, 10u);

  sa[8] ^= 1;
  EXPECT_FALSE(pickHeader(sa, sb, out));
}

TEST(BatteryRing, SlotsWrapAndHeaderAlternates) {
  EXPECT_EQ(slotOf(1), 0u);
  EXPECT_EQ(slotOf(RING_CAPACITY), RING_CAPACITY - 1);
  EXPECT_EQ(slotOf(RING_CAPACITY + 1), 0u);
  EXPECT_EQ(firstSeq(5), 1u);
  EXPECT_EQ(firstSeq(RING_CAPACITY + 10), 10u);
  EXPECT_NE(headerOffset(10), headerOffset(11));
}

TEST(BatteryLevel, NoiseIsNotCountedTwice) {
  LevelTracker t;
  const Record a = rec(1, 1, 0, 80);
  const Record b = rec(2, 1, 600, 78);
  const Record c = rec(3, 1, 1200, 79);  // ADC bounce up
  const Record d = rec(4, 1, 1800, 77);
  t.step(a, a);
  t.step(a, b);
  t.step(b, c);
  t.step(c, d);
  EXPECT_EQ(t.used, 3u);  // 80 -> 77, not 2 + 2
  EXPECT_EQ(t.charged, 0u);
}

TEST(BatteryLevel, ChargeOnUsbAndWhileOff) {
  LevelTracker t;
  const Record a = rec(1, 1, 0, 40);
  const Record b = rec(2, 1, 600, 60, 0, FLAG_USB);
  const Record c = rec(3, 1, 700, 65);  // unplugged
  const Record d = rec(4, 2, 5, 90);    // woke higher: charged while off
  t.step(a, a);
  t.step(a, b);
  t.step(b, c);
  t.step(c, d);
  EXPECT_EQ(t.charged, 50u);
  EXPECT_EQ(t.used, 0u);
}

TEST(BatteryLevel, SmallRiseAcrossSleepIsNoise) {
  const Record a = rec(1, 1, 100, 70);
  const Record b = rec(2, 2, 5, 71);
  EXPECT_FALSE(isChargeStep(a, b));
}

TEST(BatteryUsage, SplitsAwakeAndAsleep) {
  UsageAccumulator u;
  u.add(rec(1, 1, 10, 90, T0));
  u.add(rec(2, 1, 610, 88, T0 + 600, FLAG_READING));
  u.add(rec(3, 1, 1210, 86, T0 + 1200, FLAG_READING | FLAG_WIFI));
  // Slept 10 h, woke 5 s into the next boot.
  u.add(rec(4, 2, 5, 84, T0 + 1200 + 36000 + 5));
  const Usage& s = u.usage();
  EXPECT_EQ(s.awakeS, 1200u);
  EXPECT_EQ(s.readingS, 1200u);
  EXPECT_EQ(s.wifiS, 600u);
  EXPECT_EQ(s.asleepS, 36000u);
  EXPECT_EQ(s.usedAwakePct, 4u);
  EXPECT_EQ(s.usedAsleepPct, 2u);
  EXPECT_EQ(s.usedPct, 6u);
}

TEST(BatteryUsage, UndatedSleepAddsNoAsleepTime) {
  UsageAccumulator u;
  u.add(rec(1, 1, 10, 90));
  u.add(rec(2, 2, 5, 85));
  EXPECT_EQ(u.usage().asleepS, 0u);
  EXPECT_EQ(u.usage().usedPct, 5u);
}

TEST(BatteryCharge, FindsLastUnplug) {
  LastChargeFinder f;
  f.add(rec(1, 1, 0, 30));
  f.add(rec(2, 1, 600, 80, T0, FLAG_USB));
  f.add(rec(3, 1, 900, 100, T0 + 300));
  f.add(rec(4, 1, 1500, 97, T0 + 900));
  ASSERT_TRUE(f.found());
  EXPECT_EQ(f.endSeq(), 3u);
  EXPECT_EQ(f.endEpoch(), T0 + 300);
  EXPECT_EQ(f.endSoc(), 100u);
  EXPECT_FALSE(f.charging());
}

TEST(BatteryCharge, FindsChargeWhileOff) {
  LastChargeFinder f;
  f.add(rec(1, 1, 100, 20));
  f.add(rec(2, 2, 5, 95));
  ASSERT_TRUE(f.found());
  EXPECT_EQ(f.endSeq(), 2u);
  EXPECT_EQ(f.endEpoch(), 0u);  // X4 with no clock
}

TEST(BatteryBuckets, DatedPlacementAndWakeJoin) {
  Bucket b[10];
  BucketBuilder builder(b, 10);
  builder.addDated(rec(1, 1, 0, 90, T0), T0, T0 + 1000);
  builder.addDated(rec(2, 1, 50, 88, T0 + 50), T0, T0 + 1000);
  builder.addDated(rec(3, 2, 5, 80, T0 + 950, FLAG_USB), T0, T0 + 1000);
  builder.addDated(rec(4, 2, 9, 10, T0 + 2000), T0, T0 + 1000);  // outside the window
  builder.addDated(rec(5, 2, 9, 10), T0, T0 + 1000);             // undated
  EXPECT_EQ(b[0].minSoc, 88u);
  EXPECT_EQ(b[0].maxSoc, 90u);
  EXPECT_EQ(b[0].firstSoc, 90u);
  EXPECT_EQ(b[0].lastSoc, 88u);
  EXPECT_TRUE(b[1].empty());
  EXPECT_EQ(b[9].firstSoc, 80u);
  EXPECT_TRUE(b[9].flags & BUCKET_WAKE);
  EXPECT_TRUE(b[9].flags & BUCKET_CHARGING);
}

TEST(BatteryBuckets, AwakeAndChargingFillBetweenSamples) {
  Bucket b[10];
  BucketBuilder builder(b, 10);
  builder.addIndexed(rec(1, 1, 0, 50), 1, 10);
  builder.addIndexed(rec(2, 1, 600, 49), 4, 10);
  // Slept, then woke on USB and stayed on it.
  builder.addIndexed(rec(3, 2, 5, 49, 0, FLAG_USB), 6, 10);
  builder.addIndexed(rec(4, 2, 605, 60, 0, FLAG_USB), 8, 10);
  for (int c : {1, 2, 3, 4, 6, 7, 8}) EXPECT_TRUE(b[c].flags & BUCKET_AWAKE) << c;
  for (int c : {0, 5, 9}) EXPECT_FALSE(b[c].flags & BUCKET_AWAKE) << c;
  for (int c : {6, 7, 8}) EXPECT_TRUE(b[c].flags & BUCKET_CHARGING) << c;
  for (int c : {1, 2, 3, 4, 5}) EXPECT_FALSE(b[c].flags & BUCKET_CHARGING) << c;
  EXPECT_TRUE(b[7].empty());  // flagged but no sample: no line point
}

TEST(BatteryBuckets, UnknownSocIsSkipped) {
  Bucket b[4];
  BucketBuilder builder(b, 4);
  builder.addIndexed(rec(1, 1, 0, SOC_UNKNOWN), 0, 4);
  EXPECT_TRUE(b[0].empty());
  builder.addIndexed(rec(2, 1, 0, 50), 3, 4);
  EXPECT_EQ(b[3].minSoc, 50u);
}

TEST(BatteryFormat, Spans) {
  char buf[24];
  formatSpan(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "0m");
  formatSpan(5 * 3600 + 12 * 60, buf, sizeof(buf));
  EXPECT_STREQ(buf, "5h 12m");
  formatSpan(2 * 86400 + 4 * 3600 + 59, buf, sizeof(buf));
  EXPECT_STREQ(buf, "2d 4h");
}
