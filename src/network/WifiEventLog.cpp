#include "WifiEventLog.h"

#include <Logging.h>
#include <SdDebugLog.h>
#include <WiFi.h>

namespace WifiEventLog {
namespace {

// Written by the Network event task, read by UI tasks: a byte, single writer, no
// read-modify-write, so volatile is enough (a mutex is not callable from the event
// task without inverting who waits on whom).
volatile uint8_t g_lastReason = 0;
bool g_started = false;

void onStaDisconnected(arduino_event_t* event) {
  if (!event || event->event_id != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  const uint8_t reason = event->event_info.wifi_sta_disconnected.reason;
  g_lastReason = reason;
  const char* name = WiFi.STA.disconnectReasonName(static_cast<wifi_err_reason_t>(reason));
  LOG_ERR("WIFI", "STA disconnected: reason %u (%s)", static_cast<unsigned>(reason), name);
  SdDebugLog::log("WIFI", "disconnect reason=%u (%s)", static_cast<unsigned>(reason), name);
}

}  // namespace

void begin() {
  if (g_started) return;
  g_started = true;
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
}

uint8_t lastDisconnectReason() { return g_lastReason; }

const char* reasonName(const uint8_t reason) {
  const char* name = WiFi.STA.disconnectReasonName(static_cast<wifi_err_reason_t>(reason));
  return name ? name : "UNKNOWN";
}

void clear() { g_lastReason = 0; }

}  // namespace WifiEventLog
