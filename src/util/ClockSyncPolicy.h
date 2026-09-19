#pragma once

#include <HalClock.h>

#include <ctime>

#include "CrossPointSettings.h"

// When to spend an NTP round trip on a Wi-Fi connection that is already up.
//
// One place, deliberately: this decision used to be a ternary copy-pasted into
// WifiSelectionActivity and KOReaderSyncActivity, and when hasHardwareRtc() turned out not to
// know about the X4 Pro's BM8563 the same bug had to be found in both.
//
// Never powers the radio on its own. maybeStartBackgroundNtpSync() in main.cpp still refuses
// to bring Wi-Fi up just for the clock on a board that has an RTC; these helpers only decide
// whether to piggyback on a connection some other feature already made.

// Seconds a successful sync stays fresh, from SETTINGS.clockResyncDays.
inline uint32_t clockResyncIntervalSeconds() {
  const uint8_t idx = SETTINGS.clockResyncDays < CrossPointSettings::CLOCK_RESYNC_COUNT ? SETTINGS.clockResyncDays : 0;
  return static_cast<uint32_t>(CrossPointSettings::CLOCK_RESYNC_DAYS[idx]) * 86400UL;
}

inline bool clockNeedsNtpSync() {
  // No RTC: the clock dies with every deep sleep (a chip reset), so validity is the whole
  // test and the staleness rule below would only add a redundant second sync.
  if (!halClock.hasHardwareRtc()) return !halClock.isSystemTimeValid();

  // Never synced, or the RTC has stopped keeping time (dead backup cell, or one that was
  // never set). An invalid clock is always a reason to sync, whatever the latch says.
  if (!SETTINGS.clockHasBeenSynced || !halClock.isSystemTimeValid()) return true;

  // A pre-upgrade settings file has the latch set but no epoch. Treat it as due once; the
  // sync that follows writes an epoch and the normal rule takes over from there.
  if (SETTINGS.clockLastSyncEpoch == 0) return true;

  // Both directions: `now < last` means the clock moved backwards (a bad NTP packet, or a
  // manual change), which is exactly when a fresh reading is wanted rather than a wait.
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (now < SETTINGS.clockLastSyncEpoch) return true;
  return (now - SETTINGS.clockLastSyncEpoch) >= clockResyncIntervalSeconds();
}

// Record a successful sync. Writes settings only when something actually changed -- this runs
// on every Wi-Fi connect and an SD write costs serialization plus the storage mutex.
inline void noteClockSynced() {
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  bool dirty = false;
  if (!SETTINGS.clockHasBeenSynced) {
    SETTINGS.clockHasBeenSynced = 1;
    dirty = true;
  }
  // Only meaningful once the clock itself is valid; otherwise the epoch we would store is the
  // 1970 value that made the sync necessary in the first place.
  if (halClock.isSystemTimeValid() && now != SETTINGS.clockLastSyncEpoch) {
    SETTINGS.clockLastSyncEpoch = now;
    dirty = true;
  }
  if (dirty) SETTINGS.saveToFile();
}
