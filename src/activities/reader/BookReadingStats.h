#pragma once
#include <cstdint>
#include <string>

// Per-book reading statistics, persisted to cachePath/stats.bin.
struct BookReadingStats {
  uint32_t totalReadingSeconds = 0;
  // Reading time accumulated when the RTC (X3 only) wasn't available to date-stamp
  // the session. Counted in totals but excluded from the dated weekly/monthly/yearly
  // and heatmap breakdowns (see BookReadingTimeHistory).
  uint32_t unattributedSeconds = 0;
  uint16_t avgSecondsPerForwardPage = 0;
  uint16_t paceSampleCount = 0;

  // Calendar day (days-since-2000, same encoding as ReadingTimeHistory::heatmapAnchorDay
  // / readingHistoryDayIndex) and time-of-day of the most recent *dated* reading
  // session (X3 + RTC available). lastReadDayIndex == 0 means "never recorded"
  // (X4, RTC unavailable so far, or pre-v3 stats) -- UI should omit the
  // "Last read on ..." line in that case rather than show a 2000-01-01 date.
  uint32_t lastReadDayIndex = 0;
  uint8_t lastReadHour = 0;
  uint8_t lastReadMinute = 0;

  // Loads stats from cachePath/stats.bin. Returns default-constructed stats if
  // the file is missing or the version byte does not match.
  static BookReadingStats load(const std::string& cachePath);

  // Saves stats to cachePath/stats.bin.
  void save(const std::string& cachePath) const;

  // Updates the running reading pace with one forward-page dwell sample.
  void recordForwardPageRead(uint32_t seconds);

  // Formats a duration in seconds: "< 1 min", "45 min", "2h 30 min".
  static void formatDuration(uint32_t seconds, char* buf, size_t len);
};
