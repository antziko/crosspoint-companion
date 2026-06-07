#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr uint8_t GLOBAL_STATS_FILE_VERSION = 1;
constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
constexpr char GLOBAL_HISTORY_PATH[] = "/.crosspoint/global_time_history.bin";
}  // namespace

bool GlobalReadingStats::load(GlobalReadingStats& out) {
  out.totalReadingSeconds = 0;
  out.unattributedSeconds = 0;

  bool ok = true;
  HalFile f;
  if (!Storage.openFileForRead("GSTATS", GLOBAL_STATS_PATH, f)) {
    ok = false;
  } else {
    uint8_t version = 0;
    serialization::readPod(f, version);
    if (version != GLOBAL_STATS_FILE_VERSION) {
      LOG_DBG("GSTATS", "global_stats.bin missing or version mismatch, starting fresh");
      ok = false;
    } else {
      serialization::readPod(f, out.totalReadingSeconds);
      serialization::readPod(f, out.unattributedSeconds);
    }
    f.close();
  }

  ReadingTimeHistory::load(GLOBAL_HISTORY_PATH, out.history);
  return ok;
}

void GlobalReadingStats::save() const {
  HalFile f;
  if (!Storage.openFileForWrite("GSTATS", GLOBAL_STATS_PATH, f)) {
    LOG_ERR("GSTATS", "Could not write global_stats.bin");
    return;
  }
  serialization::writePod(f, GLOBAL_STATS_FILE_VERSION);
  serialization::writePod(f, totalReadingSeconds);
  serialization::writePod(f, unattributedSeconds);
  f.close();

  ReadingTimeHistory::save(GLOBAL_HISTORY_PATH, history);
}
