#pragma once
#include <cstddef>
#include <cstdint>

// Reading seconds per calendar day for this device, the data behind the Heatmap tab's week
// chart. ReadingTimeHistory keeps only a 2-bit level per day, so the exact times live here.
//
// File: u8 version, u32 anchorDay (newest recorded day index), then DAYS x u16 seconds in a
// ring where day d sits in slot d % DAYS. A slot holds day d only while
// anchorDay - DAYS < d <= anchorDay. Seconds saturate at 65535 (18 h).
namespace DailyReadingLog {
constexpr size_t DAYS = 730;  // matches ReadingTimeHistory::HEATMAP_DAYS
constexpr uint16_t MAX_SECONDS = 0xFFFF;
extern const char* const PATH;

// Adds `seconds` to day `dayIndex` (readingHistoryDayIndex). A newer day clears the slots of
// the days it skips. Rewrites the whole file through one heap buffer of DAYS * 2 bytes.
bool add(const char* path, uint32_t dayIndex, uint32_t seconds);

// Seconds for the 7 days starting at `firstDay`; 0 for days outside the window. Seeks to
// each slot, so no buffer beyond `out`. Returns false when there is no log.
bool readWeek(const char* path, uint32_t firstDay, uint16_t out[7]);
}  // namespace DailyReadingLog
