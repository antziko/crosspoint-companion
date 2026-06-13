#include "ReadingTimeHistory.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr uint8_t HISTORY_FILE_VERSION = 2;
constexpr uint8_t kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool isLeapYear(uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

// 2 bits/day, packed 4-per-byte. A day's bit-pair never crosses a byte boundary
// since 8 % 2 == 0, so no cross-byte masking is needed.
uint8_t heatmapLevelGet(const uint8_t* bits, size_t dayIndex) {
  if (dayIndex >= ReadingTimeHistory::HEATMAP_DAYS) return 0;
  const size_t bitPos = dayIndex * 2;
  return (bits[bitPos / 8] >> (bitPos % 8)) & 0x3U;
}
void heatmapLevelSet(uint8_t* bits, size_t dayIndex, uint8_t level) {
  if (dayIndex >= ReadingTimeHistory::HEATMAP_DAYS) return;
  const size_t bitPos = dayIndex * 2;
  const size_t byteIdx = bitPos / 8;
  const uint8_t shift = static_cast<uint8_t>(bitPos % 8);
  bits[byteIdx] = static_cast<uint8_t>((bits[byteIdx] & ~(0x3U << shift)) | ((level & 0x3U) << shift));
}
// Classifies a day's accumulated reading time into a HeatmapLevel ordinal.
uint8_t classifyHeatmapLevel(uint32_t seconds) {
  if (seconds == 0) return 0;
  if (seconds <= ReadingTimeHistory::HEATMAP_LIGHT_MAX_SECONDS) return 1;
  if (seconds <= ReadingTimeHistory::HEATMAP_MODERATE_MAX_SECONDS) return 2;
  return 3;
}
// Shifts every level "older" by `days` slots (slot i moves to slot i+days),
// dropping anything that falls past the end. Called when a new most-recent day
// arrives so the existing history keeps its place relative to the new anchor.
// Temp-buffer pattern (vs. in-place) avoids overwriting source slots that
// later iterations still need to read, since the shift ranges overlap.
void heatmapShiftOlder(uint8_t* bits, uint32_t days) {
  if (days == 0) return;
  if (days >= ReadingTimeHistory::HEATMAP_DAYS) {
    memset(bits, 0, ReadingTimeHistory::HEATMAP_BYTES);
    return;
  }
  uint8_t shifted[ReadingTimeHistory::HEATMAP_BYTES] = {};
  for (size_t i = 0; i + days < ReadingTimeHistory::HEATMAP_DAYS; i++) {
    const uint8_t level = heatmapLevelGet(bits, i);
    if (level != 0) heatmapLevelSet(shifted, i + days, level);
  }
  memcpy(bits, shifted, ReadingTimeHistory::HEATMAP_BYTES);
}

// Bumped only if the wire blob layout changes; independent of the on-disk
// HISTORY_FILE_VERSION (the blob omits heatmapAnchorSeconds).
constexpr uint8_t HISTORY_BLOB_VERSION = 1;

// Merges two newest-first, strictly-descending-by-key bucket arrays (the
// invariant recordDay() maintains: index 0 is the most recent bucket, no
// duplicate keys, zero-seconds tail), summing seconds where keys match and
// keeping the newest N. `dst` is overwritten with the result.
//
// `merged` is a stack temp (largest case WeekEntry[52] = 416 B). That exceeds
// the 256-byte stack-local guideline, but mergeFrom runs once per sync on the
// 4 KB sync task (not a hot/recursive path), and the three instantiations are
// sequential calls — peak frame is one array, not all three.
template <typename Entry, size_t N, typename KeyFn>
void mergeBuckets(Entry (&dst)[N], const Entry (&src)[N], KeyFn key) {
  Entry merged[N] = {};
  size_t out = 0, i = 0, j = 0;
  while (out < N) {
    const bool ai = i < N && dst[i].seconds > 0;
    const bool aj = j < N && src[j].seconds > 0;
    if (!ai && !aj) break;
    if (ai && (!aj || key(dst[i]) > key(src[j]))) {
      merged[out++] = dst[i++];
    } else if (aj && (!ai || key(src[j]) > key(dst[i]))) {
      merged[out++] = src[j++];
    } else {  // equal keys: combine into one bucket
      merged[out] = dst[i];
      merged[out].seconds += src[j].seconds;
      out++;
      i++;
      j++;
    }
  }
  memcpy(dst, merged, sizeof(merged));
}
}  // namespace

uint8_t readingHistoryDaysInMonth(uint16_t year, uint8_t month) {
  if (month < 1 || month > 12) return 30;
  uint8_t d = kDaysInMonth[month - 1];
  if (month == 2 && isLeapYear(year)) d = 29;
  return d;
}

uint8_t readingHistoryDayOfWeek(uint32_t dayIndex) {
  // Day index 0 = 2000-01-01, a Saturday (DS3231 convention 1=Sunday..7=Saturday).
  return static_cast<uint8_t>(((dayIndex % 7U) + 6U) % 7U + 1U);
}

uint32_t readingHistoryDayIndex(uint16_t year, uint8_t month, uint8_t day) {
  uint32_t idx = 0;
  for (uint16_t y = 2000; y < year; y++) idx += isLeapYear(y) ? 366U : 365U;
  for (uint8_t m = 1; m < month; m++) idx += readingHistoryDaysInMonth(year, m);
  idx += static_cast<uint32_t>(day > 0 ? day - 1 : 0);
  return idx;
}

void readingHistoryDateFromDayIndex(uint32_t dayIndex, uint16_t& year, uint8_t& month, uint8_t& day) {
  year = 2000;
  while (year < 2099) {
    const uint32_t yearDays = isLeapYear(year) ? 366U : 365U;
    if (dayIndex < yearDays) break;
    dayIndex -= yearDays;
    year++;
  }
  month = 1;
  while (month < 12) {
    const uint8_t monthDays = readingHistoryDaysInMonth(year, month);
    if (dayIndex < monthDays) break;
    dayIndex -= monthDays;
    month++;
  }
  day = static_cast<uint8_t>(dayIndex + 1);
}

void ReadingTimeHistory::recordDay(uint16_t year, uint8_t month, uint8_t day, uint8_t dayOfWeek, uint32_t seconds) {
  if (seconds == 0) return;
  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1) return;

  const uint32_t dayIdx = readingHistoryDayIndex(year, month, day);

  // Heatmap: classify this day's *accumulated* reading time into an intensity
  // level. A single calendar day can receive multiple recordDay() calls (one
  // per reading session), so the anchor slot tracks a running seconds total
  // and is reclassified from that total on every call — not set once from a
  // single session's duration (that would misclassify e.g. two 20-minute
  // sessions as "Light" instead of the correct "Moderate" 40-minute total).
  if (heatmapAnchorDay == 0 && getHeatmapLevel(0) == HeatmapLevel::None) {
    // First-ever dated entry.
    heatmapAnchorDay = dayIdx;
    heatmapAnchorSeconds = seconds;
    heatmapLevelSet(heatmapBits, 0, classifyHeatmapLevel(heatmapAnchorSeconds));
  } else if (dayIdx == heatmapAnchorDay) {
    // Same day as the running total — accumulate and reclassify.
    heatmapAnchorSeconds += seconds;
    heatmapLevelSet(heatmapBits, 0, classifyHeatmapLevel(heatmapAnchorSeconds));
  } else if (dayIdx > heatmapAnchorDay) {
    // A new most-recent day: finalize by shifting the old anchor's slot into
    // place, then start a fresh running total for this day.
    heatmapShiftOlder(heatmapBits, dayIdx - heatmapAnchorDay);
    heatmapAnchorDay = dayIdx;
    heatmapAnchorSeconds = seconds;
    heatmapLevelSet(heatmapBits, 0, classifyHeatmapLevel(heatmapAnchorSeconds));
  } else {
    // Backdated session (e.g. clock adjustment landed on a day before the
    // current anchor). We have no stored running total for past days, so this
    // is best-effort: classify just this single session and only raise the
    // existing level, never lower it — a multi-session backdated day may
    // therefore under-classify, but never display a level higher than it
    // actually earned.
    const size_t slot = heatmapAnchorDay - dayIdx;
    const uint8_t candidate = classifyHeatmapLevel(seconds);
    if (candidate > heatmapLevelGet(heatmapBits, slot)) {
      heatmapLevelSet(heatmapBits, slot, candidate);
    }
  }

  // Weekly: bucket by the Monday of this date's week. DS3231 dayOfWeek is
  // 1=Sunday..7=Saturday; convert to a Monday=0 offset to find that Monday's index.
  const uint8_t mondayOffset = static_cast<uint8_t>((static_cast<uint32_t>(dayOfWeek) + 5U) % 7U);
  uint16_t wYear;
  uint8_t wMonth, wDay;
  readingHistoryDateFromDayIndex(dayIdx - mondayOffset, wYear, wMonth, wDay);
  if (weekly[0].seconds > 0 && weekly[0].year == wYear && weekly[0].month == wMonth && weekly[0].day == wDay) {
    weekly[0].seconds += seconds;
  } else {
    for (size_t i = WEEKLY_COUNT - 1; i > 0; i--) weekly[i] = weekly[i - 1];
    weekly[0] = {wYear, wMonth, wDay, seconds};
  }

  // Monthly
  if (monthly[0].seconds > 0 && monthly[0].year == year && monthly[0].month == month) {
    monthly[0].seconds += seconds;
  } else {
    for (size_t i = MONTHLY_COUNT - 1; i > 0; i--) monthly[i] = monthly[i - 1];
    monthly[0] = {year, month, 0, seconds};
  }

  // Yearly
  if (yearly[0].seconds > 0 && yearly[0].year == year) {
    yearly[0].seconds += seconds;
  } else {
    for (size_t i = YEARLY_COUNT - 1; i > 0; i--) yearly[i] = yearly[i - 1];
    yearly[0] = {year, 0, seconds};
  }
}

ReadingTimeHistory::HeatmapLevel ReadingTimeHistory::getHeatmapLevel(size_t daysAgo) const {
  if (heatmapAnchorDay == 0 && heatmapLevelGet(heatmapBits, 0) == 0) return HeatmapLevel::None;
  return static_cast<HeatmapLevel>(heatmapLevelGet(heatmapBits, daysAgo));
}

bool ReadingTimeHistory::load(const std::string& path, ReadingTimeHistory& out) {
  out = ReadingTimeHistory{};
  HalFile f;
  if (!Storage.openFileForRead("RTH", path, f)) return false;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != HISTORY_FILE_VERSION) {
    f.close();
    LOG_DBG("RTH", "Reading history missing or version mismatch, starting fresh");
    return false;
  }
  serialization::readPod(f, out.weekly);
  serialization::readPod(f, out.monthly);
  serialization::readPod(f, out.yearly);
  serialization::readPod(f, out.heatmapBits);
  serialization::readPod(f, out.heatmapAnchorDay);
  serialization::readPod(f, out.heatmapAnchorSeconds);
  f.close();
  return true;
}

bool ReadingTimeHistory::save(const std::string& path, const ReadingTimeHistory& history) {
  HalFile f;
  if (!Storage.openFileForWrite("RTH", path, f)) {
    LOG_ERR("RTH", "Could not write %s", path.c_str());
    return false;
  }
  serialization::writePod(f, HISTORY_FILE_VERSION);
  serialization::writePod(f, history.weekly);
  serialization::writePod(f, history.monthly);
  serialization::writePod(f, history.yearly);
  serialization::writePod(f, history.heatmapBits);
  serialization::writePod(f, history.heatmapAnchorDay);
  serialization::writePod(f, history.heatmapAnchorSeconds);
  f.close();
  return true;
}

void ReadingTimeHistory::mergeFrom(const ReadingTimeHistory& other) {
  // Weekly/monthly/yearly: sum seconds by absolute date-key, keep newest N.
  mergeBuckets(weekly, other.weekly, [](const WeekEntry& e) { return readingHistoryDayIndex(e.year, e.month, e.day); });
  mergeBuckets(monthly, other.monthly,
               [](const MonthEntry& e) { return static_cast<uint32_t>(e.year) * 12U + e.month; });
  mergeBuckets(yearly, other.yearly, [](const YearEntry& e) { return static_cast<uint32_t>(e.year); });

  // Heatmap: re-anchor to the newer of the two days, then take the per-day MAX
  // level. Levels can only be promoted, never summed (no per-day seconds stored).
  const uint32_t mergedAnchor = std::max(heatmapAnchorDay, other.heatmapAnchorDay);
  if (mergedAnchor > heatmapAnchorDay) {
    // Slide our existing levels back so slot 0 lines up with the newer anchor.
    heatmapShiftOlder(heatmapBits, mergedAnchor - heatmapAnchorDay);
  }
  // other's slot s is calendar day (other.heatmapAnchorDay - s); its slot under
  // the merged anchor is offset further back by (mergedAnchor - other anchor).
  const uint32_t otherOffset = mergedAnchor - other.heatmapAnchorDay;
  for (size_t s = 0; s < HEATMAP_DAYS; s++) {
    const uint8_t lvl = heatmapLevelGet(other.heatmapBits, s);
    if (lvl == 0) continue;
    const size_t dstSlot = otherOffset + s;
    if (dstSlot >= HEATMAP_DAYS) break;  // older than our 730-day window
    if (lvl > heatmapLevelGet(heatmapBits, dstSlot)) heatmapLevelSet(heatmapBits, dstSlot, lvl);
  }
  heatmapAnchorDay = mergedAnchor;
  // heatmapAnchorSeconds intentionally left as-is: a merged history is a display
  // artifact and is never recordDay()'d onto, so the running total is moot.
}

size_t ReadingTimeHistory::serializeBlob(uint8_t* out, size_t cap) const {
  if (cap < BLOB_MAX_BYTES) return 0;
  size_t off = 0;
  out[off++] = HISTORY_BLOB_VERSION;
  // memcpy (not pointer-cast) for RISC-V alignment safety. Layout is fixed
  // little-endian; both targets (ESP32-C3) and the host test are LE with
  // identical 8-byte WeekEntry/MonthEntry/YearEntry packing.
  auto put = [&](const void* p, size_t n) {
    memcpy(out + off, p, n);
    off += n;
  };
  put(weekly, sizeof(weekly));
  put(monthly, sizeof(monthly));
  put(yearly, sizeof(yearly));
  put(heatmapBits, sizeof(heatmapBits));
  put(&heatmapAnchorDay, sizeof(heatmapAnchorDay));
  return off;
}

bool ReadingTimeHistory::deserializeBlob(const uint8_t* data, size_t len) {
  *this = ReadingTimeHistory{};
  if (len < BLOB_MAX_BYTES || data[0] != HISTORY_BLOB_VERSION) {
    LOG_DBG("RTH", "Stats history blob missing or version mismatch, ignoring");
    return false;
  }
  size_t off = 1;
  auto get = [&](void* p, size_t n) {
    memcpy(p, data + off, n);
    off += n;
  };
  get(weekly, sizeof(weekly));
  get(monthly, sizeof(monthly));
  get(yearly, sizeof(yearly));
  get(heatmapBits, sizeof(heatmapBits));
  get(&heatmapAnchorDay, sizeof(heatmapAnchorDay));
  // heatmapAnchorSeconds stays 0 (not transmitted) — fine, merge uses levels.
  return true;
}
