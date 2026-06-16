#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Per-book reading statistics, persisted to cachePath/stats.bin.
struct BookReadingStats {
  uint32_t totalReadingSeconds = 0;
  // Reading time accumulated when no clock source (X3 DS3231 RTC, or X4 NTP-synced
  // system clock) was available to date-stamp the session. Counted in totals but
  // excluded from the dated weekly/monthly/yearly and heatmap breakdowns (see
  // BookReadingTimeHistory).
  uint32_t unattributedSeconds = 0;
  uint16_t avgSecondsPerForwardPage = 0;
  uint16_t paceSampleCount = 0;

  // Calendar day (days-since-2000, same encoding as ReadingTimeHistory::heatmapAnchorDay
  // / readingHistoryDayIndex) and time-of-day of the most recent *dated* reading
  // session (clock available: X3 DS3231, or X4 NTP-synced). lastReadDayIndex == 0
  // means "never recorded" (no clock available so far, or pre-v3 stats) -- UI should
  // omit the "Last read on ..." line in that case rather than show a 2000-01-01 date.
  uint32_t lastReadDayIndex = 0;
  uint8_t lastReadHour = 0;
  uint8_t lastReadMinute = 0;

  // Sum of OTHER devices' reading seconds for this book, as last fetched from the
  // KOReader sync server (per-device monotonic counters). Never mixed into
  // totalReadingSeconds, which stays local-origin only — that separation is what
  // keeps the cross-device merge idempotent. Display total = displayTotalSeconds().
  uint32_t remoteOtherSeconds = 0;
  // Most recent dated session across OTHER devices (same day-index encoding as
  // lastReadDayIndex; 0 = none). UI should show the later of local/remote.
  uint32_t remoteLastReadDayIndex = 0;
  uint8_t remoteLastReadHour = 0;
  uint8_t remoteLastReadMinute = 0;

  // Value of totalReadingSeconds at the last successful KOReader sync of this book.
  // Used to gate the "sync before sleep" prompt: prompt only once enough reading
  // time has accrued since the last sync (totalReadingSeconds - lastSyncReadingSeconds).
  // 0 = never synced (or pre-v5 stats), so the first sync resets it.
  uint32_t lastSyncReadingSeconds = 0;

  // Total reading time across all devices (local counter + last-synced remote sum).
  uint32_t displayTotalSeconds() const { return totalReadingSeconds + remoteOtherSeconds; }

  // Parses a raw stats.bin image: v5 native; v4 accepted with lastSyncReadingSeconds
  // zeroed; v3 accepted with the remote fields + lastSyncReadingSeconds zeroed (all
  // lossless upgrades). Returns false on unknown version or size mismatch. Split out
  // from load() so host unit tests can cover the migration without SD I/O.
  static bool parse(const uint8_t* data, size_t len, BookReadingStats& out);

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
