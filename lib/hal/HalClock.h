#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  bool _ntpConfigured = false;  // set when configTzTime() called; SNTP runs async after this
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;
  mutable uint8_t _cachedDayOfWeek = 0;
  mutable uint8_t _cachedDate = 0;
  mutable uint8_t _cachedMonth = 0;
  mutable uint16_t _cachedYear = 0;
  mutable bool _hasCachedDate = false;
  mutable unsigned long _lastDatePollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // X4 only: stash the current POSIX epoch in RTC_NOINIT before a software reset
  // (the heap-defrag silent restart) so begin() can restore it on the way back up.
  // No-op if the system clock isn't NTP-valid yet, or on X3 (DS3231 persists itself).
  // RTC_NOINIT survives ESP.restart() but not power loss — same lifetime as the
  // silent-reboot flags in main.cpp.
  void persistTimeAcrossReboot() const;

  // True if time is available: DS3231 (X3) or NTP-synced POSIX clock (X4)
  bool isAvailable() const { return _available || isPosixTimeValid(); }

  // True if a hardware DS3231 RTC is present (X3 only)
  bool hasHardwareRtc() const { return _available; }

  // True if the POSIX system clock has been synced via NTP (X4 only; in-memory, lost on deep sleep)
  bool isSystemTimeValid() const { return isPosixTimeValid(); }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Get current date from DS3231. dayOfWeek is 1-7 (1=Sunday); month is 1-12.
  // Returns false if RTC is not available.
  bool getDate(uint8_t& dayOfWeek, uint8_t& date, uint8_t& month, uint16_t& year) const;

  // Format date into a caller-provided buffer. Needs >=12 bytes.
  // utcOffsetQuarterHoursBiased: same encoding as formatTime() (48 = UTC+0).
  // dateFormat: 0="30 Jun", 1="Mon, 30 Jun", 2="30/06", 3="Mon, 30/06"
  // Adjusts the displayed date by ±1 day when the offset crosses midnight.
  // Returns false if RTC is not available.
  bool formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48,
                  uint8_t dateFormat = 0) const;

  // Get the local calendar date/time: raw RTC reads with utcOffsetQuarterHoursBiased
  // applied and the date/day-of-week rolled by ±1 day when the offset crosses midnight
  // (same arithmetic as formatDate/formatTime, returned as fields instead of a string).
  // dayOfWeek is 1-7 (1=Sunday); month is 1-12; hour/minute are 0-23/0-59, already wrapped.
  // Use this -- not raw getDate()/getTime() -- for anything that buckets data by the
  // user's local calendar day (e.g. reading-history stats), so a session started just
  // after local midnight isn't attributed to the RTC's still-previous UTC day.
  // Returns false if RTC is not available.
  bool getLocalDateTime(uint8_t utcOffsetQuarterHoursBiased, uint8_t& dayOfWeek, uint8_t& date, uint8_t& month,
                        uint16_t& year, uint8_t& hour, uint8_t& minute) const;

  // Sync the DS3231 RTC from an NTP server. Requires WiFi to be connected.
  // Blocks up to maxWaitMs while waiting for SNTP (default 5s for UI callers;
  // background callers that own the WiFi connection should pass a longer budget
  // so a slow SNTP packet isn't cut off by the caller tearing WiFi down).
  // Returns true if the clock was successfully set.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP(uint32_t maxWaitMs = 5000);

 private:
  // Returns true if POSIX system clock has a plausible UTC epoch (> Jan 1 2020).
  // Checked lazily each call so the async SNTP background sync is picked up automatically
  // even if syncFromNTP() timed out while SNTP was still in progress.
  bool isPosixTimeValid() const { return _ntpConfigured && time(nullptr) > 1577836800L; }

  bool writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second);
  bool writeDateToRTC(uint8_t dayOfWeek, uint8_t date, uint8_t month, uint16_t year);
};
