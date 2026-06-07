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
  // Reading time recorded while the RTC (X3 only) wasn't available to date-stamp
  // the session (e.g. on X4, which has no clock). Counted in totals but excluded
  // from `history`'s weekly/monthly/yearly/heatmap breakdowns.
  uint32_t unattributedSeconds = 0;
  ReadingTimeHistory history;

  // Loads from /.crosspoint/global_stats.bin into `out` (a heap-allocated instance —
  // e.g. an Activity member — never a stack local: this struct is ~800 bytes).
  // Leaves `out` default-constructed (all zero) if the file is missing or its
  // version doesn't match; that's the normal "no stats recorded yet" state.
  static bool load(GlobalReadingStats& out);

  // Saves to /.crosspoint/global_stats.bin.
  void save() const;
};
