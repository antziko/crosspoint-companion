#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Dated (clock-stamped: X3 DS3231 RTC or X4 NTP-synced system clock) reading-time
// breakdown shared by per-book stats
// (cachePath/book_time_history.bin) and global stats (.crosspoint/global_stats.bin).
//
// ~880 bytes on disk / in memory — always heap-allocate via makeUniqueNoThrow<>(),
// never as an activity member or stack local (CLAUDE.md resource rule: keep stack
// frames small, big tables live on the heap).
struct ReadingTimeHistory {
  static constexpr size_t WEEKLY_COUNT = 52;
  static constexpr size_t MONTHLY_COUNT = 24;
  static constexpr size_t YEARLY_COUNT = 10;
  static constexpr size_t HEATMAP_DAYS = 730;
  // 2 bits/day (HeatmapLevel) packed into bytes — bit-pairs never cross a byte
  // boundary since 8 % 2 == 0.
  static constexpr size_t HEATMAP_BYTES = (HEATMAP_DAYS * 2 + 7) / 8;

  // Per-day reading-intensity bucket for the heatmap. Thresholds are inclusive
  // on the lower tier: seconds in (0, LIGHT] -> Light, (LIGHT, MODERATE] ->
  // Moderate, (MODERATE, inf) -> Heavy, 0 -> None.
  enum class HeatmapLevel : uint8_t { None = 0, Light = 1, Moderate = 2, Heavy = 3 };
  static constexpr uint32_t HEATMAP_LIGHT_MAX_SECONDS = 30 * 60;
  static constexpr uint32_t HEATMAP_MODERATE_MAX_SECONDS = 60 * 60;

  struct WeekEntry {
    uint16_t year = 0;
    uint8_t month = 0;  // Month of the week's Monday (1-12)
    uint8_t day = 0;    // Day-of-month of the week's Monday (1-31)
    uint32_t seconds = 0;
  };
  struct MonthEntry {
    uint16_t year = 0;
    uint8_t month = 0;  // 1-12
    uint8_t _pad = 0;
    uint32_t seconds = 0;
  };
  struct YearEntry {
    uint16_t year = 0;
    uint16_t _pad = 0;
    uint32_t seconds = 0;
  };

  // Index 0 = most recent bucket. A session that lands in a new bucket shifts
  // everything down by one and drops the oldest entry (simple ring, no random access
  // by date — the UI only ever needs "N most recent buckets").
  WeekEntry weekly[WEEKLY_COUNT] = {};
  MonthEntry monthly[MONTHLY_COUNT] = {};
  YearEntry yearly[YEARLY_COUNT] = {};

  // 730-day reading-intensity grid for the heatmap, 2 bits/day (HeatmapLevel).
  // Slot 0 corresponds to heatmapAnchorDay (the most recent recorded day);
  // slot N corresponds to heatmapAnchorDay - N.
  uint8_t heatmapBits[HEATMAP_BYTES] = {};
  uint32_t heatmapAnchorDay = 0;
  // Running reading-seconds total for the day at heatmapAnchorDay — a single
  // calendar day can receive multiple recordDay() calls (one per reading
  // session), so the slot-0 level is reclassified from this accumulated total
  // on every call, not set once. Persisted so a restart mid-day can't lose the
  // partial total and under-classify the day once it's finalized.
  uint32_t heatmapAnchorSeconds = 0;

  // Adds `seconds` of reading on the given local calendar date to the weekly,
  // monthly, yearly buckets and updates the heatmap's intensity level for that
  // day. dayOfWeek follows the DS3231 convention returned by
  // HalClock::getDate(): 1=Sunday .. 7=Saturday.
  void recordDay(uint16_t year, uint8_t month, uint8_t day, uint8_t dayOfWeek, uint32_t seconds);

  // Reading-intensity level recorded `daysAgo` days before the most recent
  // recorded day (0 = most recent day itself). None for untracked/out-of-range days.
  HeatmapLevel getHeatmapLevel(size_t daysAgo) const;

  // True if any reading was recorded `daysAgo` days before the most recent
  // recorded day — i.e. level != None. Thin convenience wrapper kept for
  // callers that only care about presence, not intensity.
  bool isHeatmapDaySet(size_t daysAgo) const { return getHeatmapLevel(daysAgo) != HeatmapLevel::None; }

  // True once at least one dated session has been recorded.
  bool hasAnyData() const { return heatmapAnchorDay != 0 || isHeatmapDaySet(0); }

  // Loads from `path`. Leaves `out` default-constructed (all zero) if the file is
  // missing or its version doesn't match — callers should treat that as "no dated
  // history yet", not an error.
  static bool load(const std::string& path, ReadingTimeHistory& out);
  static bool save(const std::string& path, const ReadingTimeHistory& history);

  // Overlays `other`'s dated buckets onto this history. Used to fold a
  // sync-snapshot of OTHER devices' reading onto the local-origin history for
  // cross-device display (KOReader stats sync). Weekly/monthly/yearly buckets
  // are summed by absolute date-key and re-truncated to the newest N; the
  // heatmap takes the per-day MAX intensity level — it stores only 2-bit levels
  // (not seconds), so levels cannot be summed, only promoted. heatmapAnchorSeconds
  // is left untouched (display reads levels only; do not recordDay() onto a merged
  // copy afterwards — it is a display artifact, not a live history).
  void mergeFrom(const ReadingTimeHistory& other);

  // Compact, self-versioned binary serialization for cross-device sync. The
  // transport layer (KOReaderSyncClient) base64-wraps the bytes into the stats
  // blob's "h" field; this struct stays free of any base64/JSON/mbedtls
  // dependency so it remains host-unit-testable. heatmapAnchorSeconds is NOT
  // serialized — only intensity levels travel, since the merge max-promotes
  // levels rather than summing seconds.
  static constexpr size_t BLOB_MAX_BYTES = 1 + sizeof(WeekEntry) * WEEKLY_COUNT + sizeof(MonthEntry) * MONTHLY_COUNT +
                                           sizeof(YearEntry) * YEARLY_COUNT + HEATMAP_BYTES + sizeof(uint32_t);
  // Writes the blob into `out` (must hold >= BLOB_MAX_BYTES). Returns bytes
  // written, or 0 if `cap` is too small.
  size_t serializeBlob(uint8_t* out, size_t cap) const;
  // Restores from a blob produced by serializeBlob(). Returns false (leaving
  // *this default-constructed) on a length/version mismatch.
  bool deserializeBlob(const uint8_t* data, size_t len);
};

// Calendar helpers used by ReadingTimeHistory's bookkeeping and by stats UI code
// that needs to format weekly/monthly labels. Day index 0 = 2000-01-01.
uint8_t readingHistoryDaysInMonth(uint16_t year, uint8_t month);
// DS3231 day-of-week convention: 1=Sunday..7=Saturday (matches HalClock::getDate()).
uint8_t readingHistoryDayOfWeek(uint32_t dayIndex);
uint32_t readingHistoryDayIndex(uint16_t year, uint8_t month, uint8_t day);
void readingHistoryDateFromDayIndex(uint32_t dayIndex, uint16_t& year, uint8_t& month, uint8_t& day);
