#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Dated (RTC-stamped) reading-time breakdown shared by per-book stats
// (cachePath/book_time_history.bin) and global stats (.crosspoint/global_stats.bin).
//
// ~785 bytes on disk / in memory — always heap-allocate via makeUniqueNoThrow<>(),
// never as an activity member or stack local (CLAUDE.md resource rule: keep stack
// frames small, big tables live on the heap).
struct ReadingTimeHistory {
  static constexpr size_t WEEKLY_COUNT = 52;
  static constexpr size_t MONTHLY_COUNT = 24;
  static constexpr size_t YEARLY_COUNT = 10;
  static constexpr size_t HEATMAP_DAYS = 730;
  static constexpr size_t HEATMAP_BYTES = (HEATMAP_DAYS + 7) / 8;

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

  // 730-day "did you read on this day" presence bitmap for the heatmap. Bit 0
  // corresponds to heatmapAnchorDay (the most recent recorded day); bit N
  // corresponds to heatmapAnchorDay - N.
  uint8_t heatmapBits[HEATMAP_BYTES] = {};
  uint32_t heatmapAnchorDay = 0;

  // Adds `seconds` of reading on the given local calendar date to the weekly,
  // monthly, yearly buckets and marks the day in the heatmap. dayOfWeek follows
  // the DS3231 convention returned by HalClock::getDate(): 1=Sunday .. 7=Saturday.
  void recordDay(uint16_t year, uint8_t month, uint8_t day, uint8_t dayOfWeek, uint32_t seconds);

  // True if the heatmap has a reading session recorded `daysAgo` days before the
  // most recent recorded day (0 = most recent day itself).
  bool isHeatmapDaySet(size_t daysAgo) const;

  // True once at least one dated session has been recorded.
  bool hasAnyData() const { return heatmapAnchorDay != 0 || isHeatmapDaySet(0); }

  // Loads from `path`. Leaves `out` default-constructed (all zero) if the file is
  // missing or its version doesn't match — callers should treat that as "no dated
  // history yet", not an error.
  static bool load(const std::string& path, ReadingTimeHistory& out);
  static bool save(const std::string& path, const ReadingTimeHistory& history);
};

// Calendar helpers used by ReadingTimeHistory's bookkeeping and by stats UI code
// that needs to format weekly/monthly labels. Day index 0 = 2000-01-01.
uint8_t readingHistoryDaysInMonth(uint16_t year, uint8_t month);
// DS3231 day-of-week convention: 1=Sunday..7=Saturday (matches HalClock::getDate()).
uint8_t readingHistoryDayOfWeek(uint32_t dayIndex);
uint32_t readingHistoryDayIndex(uint16_t year, uint8_t month, uint8_t day);
void readingHistoryDateFromDayIndex(uint32_t dayIndex, uint16_t& year, uint8_t& month, uint8_t& day);
