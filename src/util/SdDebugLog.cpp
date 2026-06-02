#include "SdDebugLog.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <cstdarg>
#include <cstdio>

namespace SdDebugLog {
namespace {
bool g_enabled = false;
// Rotate (truncate) once the log passes this size so it can't grow unbounded.
constexpr size_t MAX_LOG_BYTES = 64 * 1024;
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

}  // namespace SdDebugLog
