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
}  // namespace

namespace HangTrace {

void mark(const Task task, const Step step) {
  g_crumbs[task].step = step;
  g_crumbs[task].ms = millis();
  g_magic = kMagic;
}

void reportPreviousBoot() {
  if (g_magic == kMagic) {
    SdDebugLog::log("HANG", "prev boot: reset=%d loop=%lu@%lu render=%lu@%lu", static_cast<int>(esp_reset_reason()),
                    static_cast<unsigned long>(g_crumbs[Loop].step), static_cast<unsigned long>(g_crumbs[Loop].ms),
                    static_cast<unsigned long>(g_crumbs[Render].step), static_cast<unsigned long>(g_crumbs[Render].ms));
  }
  g_magic = 0;
}

}  // namespace HangTrace
