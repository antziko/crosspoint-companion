#include "BatteryLog.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

namespace BatteryLog {

namespace {
constexpr char DIR[] = "/.crosspoint/battery";
// Records read per SD call when iterating: 240 bytes stays under the stack budget.
constexpr uint32_t READ_CHUNK = 12;
// Undated records of this boot re-dated once the clock comes up (X4 after a Wi-Fi sync).
constexpr uint32_t BACKFILL_MAX = 200;

bool loaded = false;
bool disabled = false;  // no SD / file unusable; stop trying this boot
battery::Header hdr;
battery::Record last;  // newest record on file (seq 0 = none)
uint16_t boot = 0;
bool wakeWritten = false;
bool lastUsb = false;
uint32_t lastWriteUptimeS = 0;
uint8_t pendingFlags = 0;  // FLAG_READING / FLAG_WIFI since the previous sample
bool undatedThisBoot = false;
char path[48] = {};

uint32_t uptimeS() { return millis() / 1000; }

const char* boardSlug() {
  if (BoardConfig::isX4Pro()) return "x4pro";
  if (gpio.deviceIsX3()) return "x3";
  if (gpio.deviceIsX4()) return "x4";
  return "other";
}

uint32_t nowEpoch() {
  time_t t = 0;
  if (!halClock.utcEpoch(t) || t < static_cast<time_t>(battery::MIN_VALID_EPOCH)) return 0;
  return static_cast<uint32_t>(t);
}

bool readAt(HalFile& f, size_t off, uint8_t* buf, size_t n) {
  return f.seek(off) && f.read(buf, n) == static_cast<int>(n);
}

bool writeAt(HalFile& f, size_t off, const uint8_t* buf, size_t n) { return f.seek(off) && f.write(buf, n) == n; }

// Creates the file at full size: SdFat cannot seek past EOF, and zeroed slots decode as
// empty (seq 0).
bool createFile() {
  Storage.mkdir(DIR);
  HalFile f;
  if (!Storage.openFileForWrite("BATLOG", path, f)) return false;
  static constexpr uint8_t ZEROS[256] = {};
  size_t remaining = battery::RECORDS_OFFSET + battery::RING_CAPACITY * battery::RECORD_BYTES;
  while (remaining > 0) {
    const size_t n = std::min(remaining, sizeof(ZEROS));
    if (f.write(ZEROS, n) != n) return false;
    remaining -= n;
  }
  return true;
}

// Header lost (both slots corrupt): rebuild nextSeq from the newest record. Lifetime
// counters restart from zero.
void recoverFromRecords(HalFile& f) {
  uint8_t buf[READ_CHUNK * battery::RECORD_BYTES];
  uint32_t maxSeq = 0;
  for (uint32_t slot = 0; slot < battery::RING_CAPACITY; slot += READ_CHUNK) {
    const uint32_t n = std::min(READ_CHUNK, battery::RING_CAPACITY - slot);
    if (!readAt(f, battery::RECORDS_OFFSET + slot * battery::RECORD_BYTES, buf, n * battery::RECORD_BYTES)) break;
    for (uint32_t i = 0; i < n; ++i) {
      battery::Record r;
      if (battery::decodeRecord(buf + i * battery::RECORD_BYTES, r) && battery::slotOf(r.seq) == slot + i &&
          r.seq > maxSeq) {
        maxSeq = r.seq;
        hdr.boot = r.boot;
      }
    }
  }
  hdr.nextSeq = maxSeq + 1;
  LOG_ERR("BATLOG", "Header lost, recovered nextSeq=%lu", static_cast<unsigned long>(hdr.nextSeq));
}

bool ensureLoaded() {
  if (loaded) return true;
  if (disabled) return false;
  snprintf(path, sizeof(path), "%s/%s.bin", DIR, boardSlug());
  if (!Storage.exists(path) && !createFile()) {
    LOG_ERR("BATLOG", "Cannot create %s", path);
    disabled = true;
    return false;
  }
  HalFile f;
  if (!Storage.openFileForRead("BATLOG", path, f)) {
    disabled = true;
    return false;
  }
  uint8_t slots[battery::HEADER_BYTES * battery::HEADER_SLOTS];
  if (!readAt(f, 0, slots, sizeof(slots)) || !battery::pickHeader(slots, slots + battery::HEADER_BYTES, hdr)) {
    hdr = battery::Header{};
    recoverFromRecords(f);
  }
  if (hdr.nextSeq > 1) {
    uint8_t buf[battery::RECORD_BYTES];
    battery::Record r;
    const uint32_t seq = hdr.nextSeq - 1;
    if (readAt(f, battery::recordOffset(seq), buf, sizeof(buf)) && battery::decodeRecord(buf, r) && r.seq == seq) {
      last = r;
    }
  }
  boot = static_cast<uint16_t>(hdr.boot + 1);
  loaded = true;
  return true;
}

// Re-dates this boot's undated records from uptime once the clock is known.
void backfill(HalFile& f, const uint32_t epochNow, const uint32_t uptimeNow) {
  const uint32_t oldest = battery::firstSeq(hdr.nextSeq);
  uint8_t buf[battery::RECORD_BYTES];
  for (uint32_t seq = hdr.nextSeq - 1, n = 0; seq >= oldest && seq > 0 && n < BACKFILL_MAX; --seq, ++n) {
    battery::Record r;
    if (!readAt(f, battery::recordOffset(seq), buf, sizeof(buf)) || !battery::decodeRecord(buf, r)) break;
    if (r.seq != seq || r.boot != boot || r.dated()) break;
    if (uptimeNow < r.uptimeS || epochNow - (uptimeNow - r.uptimeS) < battery::MIN_VALID_EPOCH) break;
    r.epoch = epochNow - (uptimeNow - r.uptimeS);
    r.flags |= battery::FLAG_BACKFILLED;
    battery::encodeRecord(r, buf);
    if (!writeAt(f, battery::recordOffset(seq), buf, sizeof(buf))) break;
    if (last.seq == seq) last = r;
  }
}

void append(const battery::Event event, const bool usb, const bool requireSoc) {
  if (!ensureLoaded()) return;

  battery::Record r;
  uint16_t soc = 0;
  const bool socOk = powerManager.getBatteryPercentageChecked(soc);
  if (requireSoc && !socOk) return;
  r.soc = socOk ? static_cast<uint8_t>(std::min<uint16_t>(soc, 100)) : battery::SOC_UNKNOWN;
  r.millivolts = powerManager.getBatteryMillivolts();
  r.seq = hdr.nextSeq;
  r.epoch = nowEpoch();
  r.uptimeS = uptimeS();
  r.boot = boot;
  r.event = event;
  r.flags = pendingFlags | (usb ? battery::FLAG_USB : 0);

  HalFile f = Storage.open(path, O_RDWR);
  if (!f) {
    LOG_ERR("BATLOG", "Cannot open %s", path);
    disabled = true;
    return;
  }
  if (r.dated() && undatedThisBoot) {
    backfill(f, r.epoch, r.uptimeS);
    undatedThisBoot = false;
  }

  uint8_t buf[battery::RECORD_BYTES];
  battery::encodeRecord(r, buf);
  if (!writeAt(f, battery::recordOffset(r.seq), buf, sizeof(buf))) {
    LOG_ERR("BATLOG", "Record write failed");
    disabled = true;  // a failing card would otherwise be retried every loop
    return;
  }

  battery::Header next = hdr;
  if (last.seq != 0) {
    battery::LevelTracker t;
    t.level = hdr.level;
    t.step(last, r);
    next.level = t.level;
    next.chargedPct += t.charged;
    next.usedPct += t.used;
    if (last.boot == r.boot && r.uptimeS >= last.uptimeS) next.awakeS += r.uptimeS - last.uptimeS;
  } else if (r.socKnown()) {
    next.level = r.soc;
  }
  if (next.firstEpoch == 0 && r.dated()) next.firstEpoch = r.epoch;
  next.boot = boot;
  next.nextSeq = r.seq + 1;
  uint8_t hbuf[battery::HEADER_BYTES];
  battery::encodeHeader(next, hbuf);
  if (!writeAt(f, battery::headerOffset(next.nextSeq), hbuf, sizeof(hbuf))) {
    LOG_ERR("BATLOG", "Header write failed");
    disabled = true;
    return;
  }
  f.flush();

  hdr = next;
  last = r;
  if (!r.dated()) undatedThisBoot = true;
  pendingFlags = 0;
  lastWriteUptimeS = r.uptimeS;
  lastUsb = usb;
}
}  // namespace

void noteActivity(const bool reading, const bool wifi) {
  if (reading) pendingFlags |= battery::FLAG_READING;
  if (wifi) pendingFlags |= battery::FLAG_WIFI;
}

bool due(const bool usb) {
  if (disabled) return false;
  if (!wakeWritten) return true;
  return usb != lastUsb || uptimeS() - lastWriteUptimeS >= SAMPLE_INTERVAL_S;
}

void tick(const bool usb) {
  if (!wakeWritten) {
    // Wait for a real reading: gauge boards report a placeholder 0 until one succeeds.
    const bool graceOver = uptimeS() >= WAKE_READ_GRACE_S;
    const uint32_t before = hdr.nextSeq;
    append(battery::Event::Wake, usb, !graceOver);
    wakeWritten = hdr.nextSeq != before || disabled;
    return;
  }
  if (usb != lastUsb) {
    append(usb ? battery::Event::PlugIn : battery::Event::Unplug, usb, false);
    return;
  }
  append(battery::Event::Periodic, usb, false);
}

void onSleep(const bool usb) { append(battery::Event::Sleep, usb, false); }

bool readHeader(battery::Header& out) {
  if (!ensureLoaded()) return false;
  out = hdr;
  return true;
}

bool forEach(void (*fn)(const battery::Record& r, void* ctx), void* ctx) {
  if (!ensureLoaded()) return false;
  HalFile f;
  if (!Storage.openFileForRead("BATLOG", path, f)) return false;
  uint8_t buf[READ_CHUNK * battery::RECORD_BYTES];
  const uint32_t end = hdr.nextSeq;
  for (uint32_t seq = battery::firstSeq(end); seq < end;) {
    // Contiguous run up to the chunk size or the ring's physical end.
    const uint32_t n = std::min({READ_CHUNK, end - seq, battery::RING_CAPACITY - battery::slotOf(seq)});
    if (!readAt(f, battery::recordOffset(seq), buf, n * battery::RECORD_BYTES)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      battery::Record r;
      if (battery::decodeRecord(buf + i * battery::RECORD_BYTES, r) && r.seq == seq + i) fn(r, ctx);
    }
    seq += n;
  }
  return true;
}

}  // namespace BatteryLog
