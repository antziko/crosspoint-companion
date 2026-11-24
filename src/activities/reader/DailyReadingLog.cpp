#include "DailyReadingLog.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace DailyReadingLog {
const char* const PATH = "/.crosspoint/global_daily.bin";

namespace {
constexpr uint8_t FILE_VERSION = 1;
constexpr size_t HEADER_BYTES = 1 + sizeof(uint32_t);

bool readHeader(HalFile& f, uint32_t& anchorDay) {
  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || version != FILE_VERSION) return false;
  return f.read(&anchorDay, sizeof(anchorDay)) == static_cast<int>(sizeof(anchorDay));
}

bool inWindow(uint32_t day, uint32_t anchorDay) { return day <= anchorDay && anchorDay - day < DAYS; }
}  // namespace

bool add(const char* path, uint32_t dayIndex, uint32_t seconds) {
  if (seconds == 0) return true;
  // 1.46 KB for the rewrite: the file is small, and a whole-file rewrite keeps the write path
  // the same as ReadingTimeHistory::save. Freed on return.
  auto slots = makeUniqueNoThrow<uint16_t[]>(DAYS);
  if (!slots) {
    LOG_ERR("DRL", "OOM: daily log %u bytes", static_cast<unsigned>(DAYS * sizeof(uint16_t)));
    return false;
  }
  memset(slots.get(), 0, DAYS * sizeof(uint16_t));
  uint32_t anchorDay = 0;
  {
    HalFile f;
    if (Storage.openFileForRead("DRL", path, f)) {
      const int want = static_cast<int>(DAYS * sizeof(uint16_t));
      if (!readHeader(f, anchorDay) || f.read(slots.get(), DAYS * sizeof(uint16_t)) != want) {
        LOG_DBG("DRL", "Daily log missing or version mismatch, starting fresh");
        anchorDay = 0;
        memset(slots.get(), 0, DAYS * sizeof(uint16_t));
      }
    }
  }

  if (dayIndex > anchorDay) {
    const uint32_t skipped = dayIndex - anchorDay < DAYS ? dayIndex - anchorDay : DAYS;
    for (uint32_t i = 0; i < skipped; i++) slots[(dayIndex - i) % DAYS] = 0;
    anchorDay = dayIndex;
  } else if (!inWindow(dayIndex, anchorDay)) {
    return true;  // older than the window (clock set far back)
  }
  uint16_t& slot = slots[dayIndex % DAYS];
  slot = seconds >= static_cast<uint32_t>(MAX_SECONDS - slot) ? MAX_SECONDS : static_cast<uint16_t>(slot + seconds);

  HalFile f;
  if (!Storage.openFileForWrite("DRL", path, f)) {
    LOG_ERR("DRL", "Could not write %s", path);
    return false;
  }
  const uint8_t version = FILE_VERSION;
  f.write(&version, 1);
  f.write(reinterpret_cast<const uint8_t*>(&anchorDay), sizeof(anchorDay));
  f.write(reinterpret_cast<const uint8_t*>(slots.get()), DAYS * sizeof(uint16_t));
  return true;
}

bool readWeek(const char* path, uint32_t firstDay, uint16_t out[7]) {
  memset(out, 0, 7 * sizeof(uint16_t));
  HalFile f;
  if (!Storage.openFileForRead("DRL", path, f)) return false;
  uint32_t anchorDay = 0;
  if (!readHeader(f, anchorDay)) return false;
  for (uint32_t i = 0; i < 7; i++) {
    const uint32_t day = firstDay + i;
    if (!inWindow(day, anchorDay)) continue;
    if (!f.seek(HEADER_BYTES + (day % DAYS) * sizeof(uint16_t))) return false;
    uint16_t v = 0;
    if (f.read(&v, sizeof(v)) != static_cast<int>(sizeof(v))) return false;
    out[i] = v;
  }
  return true;
}
}  // namespace DailyReadingLog
