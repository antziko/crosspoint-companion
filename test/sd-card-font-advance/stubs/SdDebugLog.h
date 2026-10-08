#pragma once
// Host stub: the SD trace is device-only.
struct SdDebugLog {
  static void log(const char*, const char*, ...) {}
};
