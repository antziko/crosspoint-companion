#pragma once

#include <Arduino.h>

class HalClock;
extern HalClock halClock;  // Singleton

// Time source for the device clock.
//   - RTC mode (X3): time is read from the DS3231 over I2C and survives reboots.
//   - RAM mode (X4 / anything else): time is seeded by NTP into RAM and advanced with millis().
//     RAM state is lost on every cold boot and every wake from deep sleep.
class HalClock {
  bool _hasRtc = false;             // DS3231 detected on the I2C bus.
  bool _hasTime = false;            // Either RTC is present, or RAM clock has been seeded.
  bool _syncedThisSession = false;  // Set after any successful NTP sync since boot.

  // RTC-mode I2C cache (10 s, used only when _hasRtc).
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  // RAM-mode anchor (used only when !_hasRtc). All fields are wall-clock UTC at the
  // moment millis() == _ramSyncedMillis. getTime() advances them on demand.
  uint8_t _ramSyncedHour = 0;
  uint8_t _ramSyncedMinute = 0;
  uint8_t _ramSyncedSecond = 0;
  unsigned long _ramSyncedMillis = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3).
  void begin();

  // True if a DS3231 RTC is present. Drives menu visibility (RAM-only devices hide
  // the "Show (RTC)" option) and decides whether syncFromNTP() writes to hardware.
  bool hasRtc() const { return _hasRtc; }

  // True once a time source can produce a value:
  //   - RTC mode: always true after begin() succeeded.
  //   - RAM mode: true after the first successful NTP sync of the session.
  bool hasTime() const { return _hasTime; }

  // True after any successful syncFromNTP() since boot. RAM-mode debounce uses this
  // directly; RTC-mode debounce additionally consults a persisted flag managed by the caller.
  bool syncedThisSession() const { return _syncedThisSession; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if no time source is available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if no time source is available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync time from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // On success: RTC devices write the time to the DS3231; RAM devices seed the in-memory anchor.
  // Either way, syncedThisSession() becomes true.
  //
  // App-layer debouncing (skip if already synced) is enforced by the caller, not here,
  // so the HAL stays free of any settings dependency.
  bool syncFromNTP();

 private:
  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
  bool getTimeFromRtc(uint8_t& hour, uint8_t& minute) const;
  void getTimeFromRam(uint8_t& hour, uint8_t& minute) const;
};
