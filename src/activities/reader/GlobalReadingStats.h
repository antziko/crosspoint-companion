#pragma once
#include <cstdint>

#include "ReadingTimeHistory.h"

// Cumulative reading statistics across all books, persisted to
// /.crosspoint/global_stats.bin.
//
// Embeds a ReadingTimeHistory (~785 bytes) — heap-allocate this struct, never put
// it on the stack or as another struct's by-value member.
struct GlobalReadingStats {
  uint32_t totalReadingSeconds = 0;
  // Reading time recorded while no clock source (X3 DS3231 RTC, or X4 NTP-synced
  // system clock) was available to date-stamp the session. Counted in totals but
  // excluded from `history`'s weekly/monthly/yearly/heatmap breakdowns.
  uint32_t unattributedSeconds = 0;
  // Sum of OTHER devices' global reading seconds, as last fetched from the KOReader
  // sync server (per-device monotonic counters under the reserved
  // "crosspoint-global-stats" pseudo-document). Never mixed into
  // totalReadingSeconds, which stays local-origin only.
  uint32_t remoteOtherSeconds = 0;
  ReadingTimeHistory history;

  // Total reading time across all devices (local counter + last-synced remote sum).
  uint32_t displayTotalSeconds() const { return totalReadingSeconds + remoteOtherSeconds; }

  // Loads from /.crosspoint/global_stats.bin into `out` (a heap-allocated instance —
  // e.g. an Activity member — never a stack local: this struct is ~800 bytes).
  // v1 files (pre remote-sync) are upgraded losslessly with remoteOtherSeconds = 0.
  // Leaves `out` default-constructed (all zero) if the file is missing or its
  // version doesn't match; that's the normal "no stats recorded yet" state.
  static bool load(GlobalReadingStats& out);

  // Saves to /.crosspoint/global_stats.bin.
  void save() const;

  // Path-parameterized variants of load()/save() so host unit tests can exercise
  // the v1 -> v2 migration without the device's fixed /.crosspoint paths.
  static bool load(GlobalReadingStats& out, const char* statsPath, const char* historyPath);
  void save(const char* statsPath, const char* historyPath) const;
};
