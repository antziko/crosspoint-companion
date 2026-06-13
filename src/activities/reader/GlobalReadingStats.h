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
  // "crosspoint_global_stats" pseudo-document). Never mixed into
  // totalReadingSeconds, which stays local-origin only.
  uint32_t remoteOtherSeconds = 0;
  // Local-origin dated breakdown (weekly/monthly/yearly/heatmap). Updated live by
  // recordDay() during reading and uploaded verbatim — stays this device's data.
  ReadingTimeHistory history;
  // Sync-snapshot of OTHER devices' dated breakdown, summed across them. Mirrors
  // remoteOtherSeconds for the dated charts: never mixed into `history` (keeps the
  // upload idempotent/monotonic per device), only overlaid for display. Persisted
  // to its own self-versioned file (global_remote_history.bin) — no global_stats.bin
  // format change needed.
  ReadingTimeHistory remoteHistory;

  // Total reading time across all devices (local counter + last-synced remote sum).
  uint32_t displayTotalSeconds() const { return totalReadingSeconds + remoteOtherSeconds; }

  // Cross-device dated breakdown for display: local history overlaid with the
  // remote snapshot. `out` is filled (heap-allocate it; ~800 bytes). Falls back to
  // a copy of the local history when no remote snapshot has been synced.
  void displayHistory(ReadingTimeHistory& out) const {
    out = history;
    out.mergeFrom(remoteHistory);
  }

  // Loads from /.crosspoint/global_stats.bin into `out` (a heap-allocated instance —
  // e.g. an Activity member — never a stack local: this struct is ~1.7 KB).
  // v1 files (pre remote-sync) are upgraded losslessly with remoteOtherSeconds = 0.
  // Leaves `out` default-constructed (all zero) if the file is missing or its
  // version doesn't match; that's the normal "no stats recorded yet" state.
  static bool load(GlobalReadingStats& out);

  // Saves to /.crosspoint/global_stats.bin.
  void save() const;

  // Path-parameterized variants of load()/save() so host unit tests can exercise
  // migration/merge without the device's fixed /.crosspoint paths. remoteHistoryPath
  // is optional — pass nullptr to skip the remote-snapshot file (its absence just
  // leaves remoteHistory default-zeroed).
  static bool load(GlobalReadingStats& out, const char* statsPath, const char* historyPath,
                   const char* remoteHistoryPath = nullptr);
  void save(const char* statsPath, const char* historyPath, const char* remoteHistoryPath = nullptr) const;
};
