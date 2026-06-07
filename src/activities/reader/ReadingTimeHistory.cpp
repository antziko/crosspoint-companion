#include "ReadingTimeHistory.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>

namespace {
constexpr uint8_t HISTORY_FILE_VERSION = 1;
constexpr uint8_t kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool isLeapYear(uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

bool heatmapBitSet(const uint8_t* bits, size_t bitIndex) {
  if (bitIndex >= ReadingTimeHistory::HEATMAP_DAYS) return false;
  return (bits[bitIndex / 8] & static_cast<uint8_t>(1U << (bitIndex % 8))) != 0;
}
void heatmapSetBit(uint8_t* bits, size_t bitIndex) {
  if (bitIndex >= ReadingTimeHistory::HEATMAP_DAYS) return;
  bits[bitIndex / 8] |= static_cast<uint8_t>(1U << (bitIndex % 8));
}
// Shifts every set bit "older" by `days` slots (bit i moves to bit i+days),
// dropping anything that falls past the end. Called when a new most-recent day
// arrives so the existing history keeps its place relative to the new anchor.
void heatmapShiftOlder(uint8_t* bits, uint32_t days) {
  if (days == 0) return;
  if (days >= ReadingTimeHistory::HEATMAP_DAYS) {
    memset(bits, 0, ReadingTimeHistory::HEATMAP_BYTES);
    return;
  }
  uint8_t shifted[ReadingTimeHistory::HEATMAP_BYTES] = {};
  for (size_t i = 0; i + days < ReadingTimeHistory::HEATMAP_DAYS; i++) {
    if (heatmapBitSet(bits, i)) heatmapSetBit(shifted, i + days);
  }
  memcpy(bits, shifted, ReadingTimeHistory::HEATMAP_BYTES);
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

  // Heatmap: mark presence for this day, sliding the anchor forward if this is
  // the most recent day seen yet.
  if (heatmapAnchorDay == 0 && !heatmapBitSet(heatmapBits, 0)) {
    heatmapAnchorDay = dayIdx;
    heatmapSetBit(heatmapBits, 0);
  } else if (dayIdx > heatmapAnchorDay) {
    heatmapShiftOlder(heatmapBits, dayIdx - heatmapAnchorDay);
    heatmapAnchorDay = dayIdx;
    heatmapSetBit(heatmapBits, 0);
  } else {
    heatmapSetBit(heatmapBits, heatmapAnchorDay - dayIdx);
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

bool ReadingTimeHistory::isHeatmapDaySet(size_t daysAgo) const {
  if (heatmapAnchorDay == 0 && !heatmapBitSet(heatmapBits, 0)) return false;
  return heatmapBitSet(heatmapBits, daysAgo);
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
  f.close();
  return true;
}
