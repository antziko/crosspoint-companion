#include "HangTrace.h"

#include <Arduino.h>
#include <SdDebugLog.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace {
constexpr uint32_t kMagic = 0x48545243;  // "HTRC"
struct Crumb {
  uint32_t step;
  uint32_t ms;
};
RTC_NOINIT_ATTR uint32_t g_magic;
RTC_NOINIT_ATTR Crumb g_crumbs[2];
// The previous boot's crumbs, copied before this boot's tasks start overwriting them.
bool g_havePrev = false;
Crumb g_prev[2];
}  // namespace

namespace HangTrace {

void mark(const Task task, const Step step) {
  g_crumbs[task].step = step;
  g_crumbs[task].ms = millis();
  g_magic = kMagic;
}

void capturePreviousBoot() {
  g_havePrev = g_magic == kMagic;
  if (g_havePrev) {
    g_prev[Loop] = g_crumbs[Loop];
    g_prev[Render] = g_crumbs[Render];
  }
  g_magic = 0;
}

void reportPreviousBoot() {
  const int reason = static_cast<int>(esp_reset_reason());
  if (g_havePrev) {
    SdDebugLog::log("HANG", "prev boot: reset=%d loop=%lu@%lu render=%lu@%lu", reason,
                    static_cast<unsigned long>(g_prev[Loop].step), static_cast<unsigned long>(g_prev[Loop].ms),
                    static_cast<unsigned long>(g_prev[Render].step), static_cast<unsigned long>(g_prev[Render].ms));
  } else {
    // Power-on resets lose RTC memory, so no crumbs survive them.
    SdDebugLog::log("HANG", "prev boot: reset=%d no crumbs", reason);
  }
}

}  // namespace HangTrace
