#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    _available = false;
    return;
  }
  Wire.read();  // discard — just testing connectivity

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Wire.read();  // seconds — not needed
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::getDate(uint8_t& dayOfWeek, uint8_t& date, uint8_t& month, uint16_t& year) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_hasCachedDate && _lastDatePollMs != 0 && (now - _lastDatePollMs) < CLOCK_POLL_MS) {
    dayOfWeek = _cachedDayOfWeek;
    date      = _cachedDate;
    month     = _cachedMonth;
    year      = _cachedYear;
    return true;
  }

  // Read 4 bytes starting at DS3231_DOW_REG: day-of-week, date, month, year
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_DOW_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedDate) return false;
    dayOfWeek = _cachedDayOfWeek;
    date      = _cachedDate;
    month     = _cachedMonth;
    year      = _cachedYear;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)4);
  if (Wire.available() < 4) {
    if (!_hasCachedDate) return false;
    dayOfWeek = _cachedDayOfWeek;
    date      = _cachedDate;
    month     = _cachedMonth;
    year      = _cachedYear;
    return true;
  }

  const uint8_t rawDow  = Wire.read();  // 0x03: day-of-week
  const uint8_t rawDate = Wire.read();  // 0x04: date
  const uint8_t rawMon  = Wire.read();  // 0x05: month (bit 7 = century)
  const uint8_t rawYear = Wire.read();  // 0x06: year (0-99)

  _cachedDayOfWeek = bcdToDec(rawDow & 0x07);
  _cachedDate      = bcdToDec(rawDate & 0x3F);
  _cachedMonth     = bcdToDec(rawMon & 0x1F);
  _cachedYear      = 2000 + bcdToDec(rawYear);
  _hasCachedDate   = true;
  _lastDatePollMs  = now;

  dayOfWeek = _cachedDayOfWeek;
  date      = _cachedDate;
  month     = _cachedMonth;
  year      = _cachedYear;
  return true;
}

static uint8_t daysInMonth(uint8_t month, uint16_t year) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  uint8_t d = kDays[month - 1];
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) d = 29;
  return d;
}

bool HalClock::getLocalDateTime(uint8_t utcOffsetQuarterHoursBiased, uint8_t& dayOfWeek, uint8_t& date,
                                uint8_t& month, uint16_t& year, uint8_t& hour, uint8_t& minute) const {
  uint8_t dow, rawDate, mo, h, m;
  uint16_t yr;
  if (!getDate(dow, rawDate, mo, yr)) return false;
  if (!getTime(h, m)) return false;
  if (mo < 1 || mo > 12) return false;

  // Same offset+rollover arithmetic as formatDate (HalClock.cpp:212-244) and
  // VegaTheme::formatLastRead, factored out here so callers that need to *bucket*
  // data by local day (not just display it) get one shared, tested implementation.
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  const int offsetMins = (static_cast<int>(utcOffsetQuarterHoursBiased) - 48) * 15;
  int localMins = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetMins;

  int d = static_cast<int>(rawDate);
  uint8_t wd = dow;  // DS3231: 1=Sunday .. 7=Saturday

  if (localMins < 0) {
    localMins += 1440;
    if (--d < 1) {
      if (--mo < 1) { mo = 12; yr--; }
      d = daysInMonth(mo, yr);
    }
    wd = static_cast<uint8_t>(((static_cast<int>(wd) - 2 + 7) % 7) + 1);
  } else if (localMins >= 1440) {
    localMins -= 1440;
    if (++d > daysInMonth(mo, yr)) {
      d = 1;
      if (++mo > 12) { mo = 1; yr++; }
    }
    wd = static_cast<uint8_t>((wd % 7) + 1);
  }

  dayOfWeek = wd;
  date = static_cast<uint8_t>(d);
  month = mo;
  year = yr;
  hour = static_cast<uint8_t>(localMins / 60);
  minute = static_cast<uint8_t>(localMins % 60);
  return true;
}

bool HalClock::formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased,
                          uint8_t dateFormat) const {
  if (!buf || bufSize < 4) return false;
  uint8_t dow, rawDate, month;
  uint16_t year;
  uint8_t h, m;
  if (!getDate(dow, rawDate, month, year)) return false;
  if (!getTime(h, m)) return false;
  if (month < 1 || month > 12) return false;

  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  const int offsetMins = (static_cast<int>(utcOffsetQuarterHoursBiased) - 48) * 15;
  const int localMins = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetMins;

  int d = static_cast<int>(rawDate);
  uint8_t mo = month;
  uint16_t yr = year;
  uint8_t wd = dow;  // DS3231: 1=Sunday .. 7=Saturday

  if (localMins < 0) {
    if (--d < 1) {
      if (--mo < 1) { mo = 12; yr--; }
      d = daysInMonth(mo, yr);
    }
    wd = static_cast<uint8_t>(((static_cast<int>(wd) - 2 + 7) % 7) + 1);
  } else if (localMins >= 1440) {
    if (++d > daysInMonth(mo, yr)) {
      d = 1;
      if (++mo > 12) { mo = 1; yr++; }
    }
    wd = static_cast<uint8_t>((wd % 7) + 1);
  }

  static const char* const kMonthNames[12] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const char* const kDowNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

  const char* mon = kMonthNames[mo - 1];
  const char* dow3 = (wd >= 1 && wd <= 7) ? kDowNames[wd - 1] : "???";
  const unsigned ud = static_cast<unsigned>(d);
  const unsigned umo = static_cast<unsigned>(mo);

  switch (dateFormat) {
    default:
    case 0: snprintf(buf, bufSize, "%u %s", ud, mon); break;                    // 30 Jun
    case 1: snprintf(buf, bufSize, "%s, %u %s", dow3, ud, mon); break;          // Mon, 30 Jun
    case 2: snprintf(buf, bufSize, "%02u/%02u", ud, umo); break;                // 30/06
    case 3: snprintf(buf, bufSize, "%s, %02u/%02u", dow3, ud, umo); break;      // Mon, 30/06
  }
  return true;
}

bool HalClock::writeDateToRTC(uint8_t dayOfWeek, uint8_t date, uint8_t month, uint16_t year) {
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_DOW_REG);
  Wire.write(decToBcd(dayOfWeek));
  Wire.write(decToBcd(date));
  Wire.write(decToBcd(month));
  Wire.write(decToBcd(static_cast<uint8_t>(year % 100)));
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write date to DS3231");
    return false;
  }
  _lastDatePollMs  = 0;  // Invalidate date poll timer so next getDate() reads fresh
  _cachedDayOfWeek = dayOfWeek;
  _cachedDate      = date;
  _cachedMonth     = month;
  _cachedYear      = year;
  _hasCachedDate   = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (!writeTimeToRTC(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec)) {
        return false;
      }
      LOG_INF("CLK", "RTC set to %02d:%02d:%02d UTC", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

      // Also write the date — tm_wday is 0=Sunday, DS3231 uses 1-based (1=Sunday)
      const uint8_t dow = static_cast<uint8_t>(timeinfo.tm_wday + 1);
      const uint8_t day = static_cast<uint8_t>(timeinfo.tm_mday);
      const uint8_t mon = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      const uint16_t yr = static_cast<uint16_t>(1900 + timeinfo.tm_year);
      if (!writeDateToRTC(dow, day, mon, yr)) {
        LOG_ERR("CLK", "NTP time synced but date write failed (non-fatal)");
      } else {
        LOG_INF("CLK", "RTC date set to %04u-%02u-%02u", (unsigned)yr, (unsigned)mon, (unsigned)day);
      }
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
