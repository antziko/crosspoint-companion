#include "TrustedTime.h"

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <Preferences.h>
#include <WiFi.h>

#include <ctime>

namespace trustedtime {

namespace {

// Below this the clock was obviously never set (2025-01-01 UTC).
constexpr int64_t MIN_VALID_EPOCH = 1735689600LL;
// NVS wear guard: only rewrite the floor when it moved by at least this much.
constexpr int64_t MIN_ADVANCE_SECS = 60;

constexpr const char* PREFS_NAMESPACE = "cptime";
constexpr const char* PREFS_KEY = "floor";

// Latest time this boot has seen (or restored from NVS). trustedNow() never
// reports earlier, so a backward clock step (a bad SNTP answer, a manual set)
// cannot reopen an expired loan. Updated from the SNTP callback's lwIP task as
// well as the main task; 64-bit atomics are not lock-free on the C3.
int64_t ramFloor = 0;
portMUX_TYPE ramFloorLock = portMUX_INITIALIZER_UNLOCKED;

// Raises the in-RAM floor to `value` if higher; returns the resulting floor.
int64_t raiseFloor(const int64_t value) {
  portENTER_CRITICAL(&ramFloorLock);
  if (value > ramFloor) ramFloor = value;
  const int64_t floor = ramFloor;
  portEXIT_CRITICAL(&ramFloorLock);
  return floor;
}

int64_t readFloor() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true)) return 0;
  const int64_t value = prefs.getLong64(PREFS_KEY, 0);
  prefs.end();
  return value;
}

void writeFloor(const int64_t value) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false)) return;
  prefs.putLong64(PREFS_KEY, value);
  prefs.end();
}

}  // namespace

// The floor is restored into RAM only, never into the system clock: HalClock adopts any
// plausible POSIX time after a soft reset as synced, so a restored floor would show a stale
// wall clock and suppress the next NTP sync.
void init() {
  const int64_t floor = readFloor();
  if (floor < MIN_VALID_EPOCH) return;
  raiseFloor(floor);
  LOG_DBG("TIME", "Restored clock floor");
}

void note() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (now < MIN_VALID_EPOCH) return;
  raiseFloor(now);
  if (now - readFloor() >= MIN_ADVANCE_SECS) writeFloor(now);
}

// Sync goes through HalClock, which hands SNTP IP literals: a hostname server leaves a DNS
// query parked in lwIP that later panics in udp_new_ip_type on the X3.
void startSync() {
  if (halClock.isSystemTimeValid()) {
    note();
    return;
  }
  syncNow(3000);
}

bool syncNow(const uint32_t timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) return false;
  const bool synced = halClock.syncFromNTP(timeoutMs);
  if (synced) note();
  return synced;
}

int64_t trustedNow() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  // Never earlier than a time already seen; 0 while neither is trustworthy.
  const int64_t floor = raiseFloor(now >= MIN_VALID_EPOCH ? now : 0);
  return floor >= MIN_VALID_EPOCH ? floor : 0;
}

}  // namespace trustedtime
