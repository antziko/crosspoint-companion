#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
// v2 appends remoteOtherSeconds (KOReader cross-device stats sync). v1 is v2's
// leading layout; load() upgrades it losslessly by zeroing the new field.
constexpr uint8_t GLOBAL_STATS_FILE_VERSION = 2;
constexpr uint8_t GLOBAL_STATS_FILE_VERSION_V1 = 1;
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr char GLOBAL_HISTORY_PATH[] = "/.crosspoint/global_time_history.bin";
}  // namespace

bool GlobalReadingStats::load(GlobalReadingStats& out, const char* statsPath, const char* historyPath) {
  out.totalReadingSeconds = 0;
  out.unattributedSeconds = 0;
  out.remoteOtherSeconds = 0;

  bool ok = true;
  HalFile f;
  if (!Storage.openFileForRead("GSTATS", statsPath, f)) {
    ok = false;
  } else {
    uint8_t version = 0;
    serialization::readPod(f, version);
    if (version != GLOBAL_STATS_FILE_VERSION && version != GLOBAL_STATS_FILE_VERSION_V1) {
      LOG_DBG("GSTATS", "global_stats.bin missing or version mismatch, starting fresh");
      ok = false;
    } else {
      serialization::readPod(f, out.totalReadingSeconds);
      serialization::readPod(f, out.unattributedSeconds);
      if (version == GLOBAL_STATS_FILE_VERSION) {
        serialization::readPod(f, out.remoteOtherSeconds);
      }
      // v1: remoteOtherSeconds stays 0 — field predates the stats sync.
    }
    f.close();
  }

  ReadingTimeHistory::load(historyPath, out.history);
  return ok;
}

bool GlobalReadingStats::load(GlobalReadingStats& out) { return load(out, GLOBAL_STATS_PATH, GLOBAL_HISTORY_PATH); }

void GlobalReadingStats::save(const char* statsPath, const char* historyPath) const {
  HalFile f;
  if (!Storage.openFileForWrite("GSTATS", statsPath, f)) {
    LOG_ERR("GSTATS", "Could not write global_stats.bin");
    return;
  }
  serialization::writePod(f, GLOBAL_STATS_FILE_VERSION);
  serialization::writePod(f, totalReadingSeconds);
  serialization::writePod(f, unattributedSeconds);
  serialization::writePod(f, remoteOtherSeconds);
  f.close();

  ReadingTimeHistory::save(historyPath, history);
}

void GlobalReadingStats::save() const { save(GLOBAL_STATS_PATH, GLOBAL_HISTORY_PATH); }
