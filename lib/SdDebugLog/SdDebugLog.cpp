#include "SdDebugLog.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include <cstdarg>
#include <cstdio>

namespace SdDebugLog {
namespace {
bool g_enabled = false;
// Master switch from the user setting. Defaults true so boot logging (before
// settings load) and TRACE_HEAP builds emit; set to SETTINGS.sdCardLogging once
// settings load. Plain bool: written from UI/web task, read in log() from network/
// render tasks — single-byte store, atomic on RISC-V, no barrier needed (it is a
// write-enable, not shared buffer state).
bool g_masterEnabled = true;
// Rotate (truncate) once the log passes this size so it can't grow unbounded.
// Sized for an extended X3 HTTPS troubleshooting collection run (many transfer
// attempts over "some time") rather than a single-session trace — SD card space
// is not the constrained resource here (RAM is); 4MB is a blink to write/read
// and holds tens of thousands of lines.
constexpr size_t MAX_LOG_BYTES = 4 * 1024 * 1024;

// Lines whose append failed (card not answering). Reported by the next line that lands, so a
// silent gap in the trace reads as "SD stopped taking writes" rather than "nothing happened".
// Only the caller of log() touches these; concurrent loggers can at worst miscount, never corrupt.
uint32_t g_droppedLines = 0;
uint32_t g_firstDropMs = 0;
uint32_t g_lastDropMs = 0;
char g_firstDropTag[8] = {};

// A card that stops answering makes every open wait out SD timeouts. After this many failures in
// a row, stop touching the card for BACKOFF_MS and only count, so logging can't add that wait to
// every page turn and lookup while the card is sick.
constexpr uint8_t FAILS_BEFORE_BACKOFF = 3;
constexpr uint32_t BACKOFF_MS = 10000;
uint8_t g_consecutiveFails = 0;
uint32_t g_backoffUntilMs = 0;

void noteDropped(const char* tag) {
  const uint32_t now = millis();
  if (g_droppedLines == 0) {
    g_firstDropMs = now;
    snprintf(g_firstDropTag, sizeof(g_firstDropTag), "%s", tag);
  }
  g_lastDropMs = now;
  g_droppedLines++;
}

void noteWriteFailed(const char* tag) {
  noteDropped(tag);
  if (++g_consecutiveFails >= FAILS_BEFORE_BACKOFF) {
    g_backoffUntilMs = millis() + BACKOFF_MS;
    g_consecutiveFails = 0;
  }
}
}  // namespace

void setEnabled(bool enabled) { g_enabled = enabled; }
bool isEnabled() { return g_enabled; }
void setMasterEnabled(bool enabled) { g_masterEnabled = enabled; }
bool isMasterEnabled() { return g_masterEnabled; }

void clear() { Storage.remove(PATH); }

void log(const char* tag, const char* fmt, ...) {
  if (!g_masterEnabled || !g_enabled) return;

  // Format the message body into a stack buffer (keep stack use < 256 B/line).
  char msg[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  // Device tag (X3/X4) on every line so a mixed-device trace stays separable —
  // the HTTPS heap behaviour differs between the X3 (96KB dual framebuffer) and
  // X4 (48KB single). gpio is initialised at boot; all network logging is runtime.
  const bool isX4 = gpio.deviceIsX4();
  const char* model = isX4 ? "X4" : "X3";

  // Serial mirror is X4-only: the X3 is USB-locked (no usable serial), so only the
  // X4 emits the live trace. SD logging below is unconditional and runs on BOTH
  // devices. (LOG_INF also compiles to nothing without ENABLE_SERIAL_LOG.)
  if (isX4) {
    LOG_INF(tag, "[%s] %s", model, msg);
  }

  char line[256];
  const int len = snprintf(line, sizeof(line), "[%lu][%s] %s: %s\n", millis(), model, tag, msg);
  if (len <= 0) return;

  if (g_backoffUntilMs != 0) {
    if (static_cast<int32_t>(millis() - g_backoffUntilMs) < 0) {
      noteDropped(tag);
      return;
    }
    g_backoffUntilMs = 0;
  }

  HalFile file;
  if (!Storage.openFileForAppend("SDLOG", PATH, file)) {
    noteWriteFailed(tag);
    return;
  }
  // Rotate once the log passes the cap. The append handle already knows the size, so no
  // separate probe open per line.
  if (file.size() > MAX_LOG_BYTES) {
    file.close();
    Storage.remove(PATH);
    if (!Storage.openFileForAppend("SDLOG", PATH, file)) {
      noteWriteFailed(tag);
      return;
    }
  }
  if (g_droppedLines > 0) {
    // msg is already copied into line; reuse it rather than grow this frame.
    const int gapLen = snprintf(msg, sizeof(msg), "[%lu][%s] SDLOG: dropped %lu lines %lu..%lu first=%s\n", millis(),
                                model, (unsigned long)g_droppedLines, (unsigned long)g_firstDropMs,
                                (unsigned long)g_lastDropMs, g_firstDropTag);
    if (gapLen > 0) file.write(msg, static_cast<size_t>(gapLen));
    g_droppedLines = 0;
  }
  if (file.write(line, static_cast<size_t>(len)) != static_cast<size_t>(len)) {
    noteWriteFailed(tag);
  } else {
    g_consecutiveFails = 0;
  }
  // Force the write (data + dir entry + FAT) to the card NOW. The activities that
  // enable this log (OPDS / KOSync) silent-restart on exit, and an abrupt
  // ESP.restart() before the close-sync lands drops a freshly-created file — which
  // is exactly why /opds_debug.txt never appeared on the X4 despite log() running.
  file.flush();
  // HalFile closes on scope exit (DESTRUCTOR_CLOSES_FILE).
}

NetSnapshot captureNetSnapshot() {
  NetSnapshot snap{};
  snap.heapFree = ESP.getFreeHeap();
  snap.largest8Bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  snap.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  snap.internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  wifi_ap_record_t apInfo = {};
  snap.rssi = (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) ? apInfo.rssi : 0;
  return snap;
}

}  // namespace SdDebugLog
