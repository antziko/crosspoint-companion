#include "SdDebugLog.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include <cstdarg>
#include <cstdio>

namespace SdDebugLog {
namespace {
bool g_enabled = false;
// Rotate (truncate) once the log passes this size so it can't grow unbounded.
// Sized for an extended X3 HTTPS troubleshooting collection run (many transfer
// attempts over "some time") rather than a single-session trace — SD card space
// is not the constrained resource here (RAM is); 4MB is a blink to write/read
// and holds tens of thousands of lines.
constexpr size_t MAX_LOG_BYTES = 4 * 1024 * 1024;
}  // namespace

void setEnabled(bool enabled) { g_enabled = enabled; }
bool isEnabled() { return g_enabled; }

void clear() { Storage.remove(PATH); }

void log(const char* tag, const char* fmt, ...) {
  if (!g_enabled) return;

  // Format the message body into a stack buffer (keep stack use < 256 B/line).
  char msg[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  char line[256];
  const int len = snprintf(line, sizeof(line), "[%lu] %s: %s\n", millis(), tag, msg);
  if (len <= 0) return;

  // Rotate if the file has grown too large (cheap size check before append).
  HalFile probe;
  if (Storage.openFileForRead("SDLOG", PATH, probe)) {
    const size_t sz = probe.size();
    probe.close();
    if (sz > MAX_LOG_BYTES) Storage.remove(PATH);
  }

  HalFile file;
  if (!Storage.openFileForAppend("SDLOG", PATH, file)) return;
  file.write(line, static_cast<size_t>(len));
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
