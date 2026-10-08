#include "LoanDue.h"

#include <I18n.h>
#include <TrustedTime.h>

#include <cstdio>
#include <ctime>

namespace loandue {

namespace {
constexpr int64_t SECONDS_PER_DAY = 24 * 60 * 60;
constexpr int64_t REMINDER_WINDOW = 3 * SECONDS_PER_DAY;

// Seconds until the loan ends; false when not on loan or the clock is unknown.
bool secondsLeft(const int64_t expiresAt, int64_t& left) {
  if (expiresAt <= 0) return false;
  const int64_t now = trustedtime::trustedNow();
  if (now <= 0) return false;
  left = expiresAt - now;
  return true;
}
}  // namespace

bool dueSoon(const int64_t expiresAt) {
  int64_t left = 0;
  return secondsLeft(expiresAt, left) && left > 0 && left <= REMINDER_WINDOW;
}

bool describe(const int64_t expiresAt, char* buf, const size_t size) {
  int64_t left = 0;
  if (!buf || size == 0 || !secondsLeft(expiresAt, left) || left <= 0) return false;

  // Same day/month form as HalClock::formatDate's default, in local time.
  static constexpr const char* MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const time_t due = static_cast<time_t>(expiresAt);
  struct tm local = {};
  localtime_r(&due, &local);
  char date[16];
  snprintf(date, sizeof(date), "%d %s", local.tm_mday, MONTHS[local.tm_mon % 12]);

  if (left >= 2 * SECONDS_PER_DAY) {
    snprintf(buf, size, tr(STR_LOAN_DUE_DAYS), date, static_cast<int>(left / SECONDS_PER_DAY));
  } else if (left >= SECONDS_PER_DAY) {
    snprintf(buf, size, tr(STR_LOAN_DUE_ONE_DAY), date);
  } else {
    const int hours = static_cast<int>(left / 3600);
    snprintf(buf, size, tr(STR_LOAN_DUE_HOURS), date, hours < 1 ? 1 : hours);
  }
  return true;
}

}  // namespace loandue
