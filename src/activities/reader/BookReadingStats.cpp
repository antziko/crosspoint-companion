#include "BookReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {
// Binary layout v3 (19 bytes):
//   [0]     version (= 3)
//   [1-4]   totalReadingSeconds       uint32_t LE
//   [5-8]   unattributedSeconds       uint32_t LE
//   [9-10]  avgSecondsPerForwardPage  uint16_t LE
//   [11-12] paceSampleCount           uint16_t LE
//   [13-16] lastReadDayIndex          uint32_t LE  (0 = never recorded)
//   [17]    lastReadHour              uint8_t
//   [18]    lastReadMinute            uint8_t
//
// v1 (15 bytes, sessionCount/totalPagesTurned) and v2 (13 bytes, no last-read
// fields) files are rejected by the version check below and replaced with fresh
// v3 stats. v1's two counters were dropped entirely; v2's fields all carry over
// 1:1 into v3's leading layout, but the version bump means existing v2 readers
// simply start over with sentinel last-read fields -- nothing to migrate.
constexpr uint8_t STATS_FILE_VERSION = 3;
constexpr int STATS_FILE_SIZE = 19;
constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;

uint16_t readLe16(const uint8_t* d, int o) {
  return static_cast<uint16_t>(d[o]) | (static_cast<uint16_t>(d[o + 1]) << 8);
}
uint32_t readLe32(const uint8_t* d, int o) {
  return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) |
         (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}
void writeLe16(uint8_t* d, int o, uint16_t v) {
  d[o] = v & 0xFF;
  d[o + 1] = (v >> 8) & 0xFF;
}
void writeLe32(uint8_t* d, int o, uint32_t v) {
  d[o] = v & 0xFF;
  d[o + 1] = (v >> 8) & 0xFF;
  d[o + 2] = (v >> 16) & 0xFF;
  d[o + 3] = (v >> 24) & 0xFF;
}
}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  HalFile f;
  if (!Storage.openFileForRead("STATS", cachePath + "/stats.bin", f)) {
    return stats;
  }
  uint8_t data[STATS_FILE_SIZE] = {};
  const int n = f.read(data, STATS_FILE_SIZE);
  f.close();
  if (n != STATS_FILE_SIZE || data[0] != STATS_FILE_VERSION) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  stats.totalReadingSeconds = readLe32(data, 1);
  stats.unattributedSeconds = readLe32(data, 5);
  stats.avgSecondsPerForwardPage = readLe16(data, 9);
  stats.paceSampleCount = readLe16(data, 11);
  stats.lastReadDayIndex = readLe32(data, 13);
  stats.lastReadHour = data[17];
  stats.lastReadMinute = data[18];
  return stats;
}

void BookReadingStats::save(const std::string& cachePath) const {
  HalFile f;
  if (!Storage.openFileForWrite("STATS", cachePath + "/stats.bin", f)) {
    LOG_ERR("STATS", "Could not write stats.bin");
    return;
  }
  uint8_t data[STATS_FILE_SIZE];
  memset(data, 0, sizeof(data));
  data[0] = STATS_FILE_VERSION;
  writeLe32(data, 1, totalReadingSeconds);
  writeLe32(data, 5, unattributedSeconds);
  writeLe16(data, 9, avgSecondsPerForwardPage);
  writeLe16(data, 11, paceSampleCount);
  writeLe32(data, 13, lastReadDayIndex);
  data[17] = lastReadHour;
  data[18] = lastReadMinute;
  f.write(data, STATS_FILE_SIZE);
  f.close();
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) return;
  if (seconds > UINT16_MAX) seconds = UINT16_MAX;
  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }
  const uint16_t weight = paceSampleCount < MAX_PACE_SAMPLE_COUNT ? paceSampleCount : MAX_PACE_SAMPLE_COUNT;
  const uint32_t nextAvg =
      (static_cast<uint32_t>(avgSecondsPerForwardPage) * weight + sample) / (static_cast<uint32_t>(weight) + 1U);
  avgSecondsPerForwardPage = static_cast<uint16_t>(nextAvg);
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) paceSampleCount++;
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%lus", static_cast<unsigned long>(seconds));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lum", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}
