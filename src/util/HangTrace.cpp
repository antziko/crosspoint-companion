#include "HangTrace.h"

#include <Arduino.h>
#include <SdDebugLog.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
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
constexpr uint32_t kStallMs = 5000;
constexpr uint32_t kStallRepeatMs = 10000;
// The render crumb last reported as stalled, and when; a new step or start time reports afresh.
Crumb g_stallSeen = {0, 0};
uint32_t g_stallLoggedMs = 0;
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

void pollLiveStall() {
  // Written by the render task one word at a time; a torn pair only skews one diagnostic line.
  const Crumb c = g_crumbs[Render];
  if (c.step == RenderWait || c.step == DwsDone || c.step == 0) return;
  const uint32_t now = millis();
  const uint32_t held = now - c.ms;
  if (held < kStallMs) return;
  const bool sameStall = g_stallSeen.step == c.step && g_stallSeen.ms == c.ms;
  if (sameStall && now - g_stallLoggedMs < kStallRepeatMs) return;
  g_stallSeen = c;
  g_stallLoggedMs = now;
  SdDebugLog::log("HANG", "live render step=%lu for %lums free=%u largest=%u", static_cast<unsigned long>(c.step),
                  static_cast<unsigned long>(held), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

}  // namespace HangTrace
