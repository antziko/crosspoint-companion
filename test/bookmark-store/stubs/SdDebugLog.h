#pragma once
// Host stub: the SD trace is device-only. Swallows the call so BookmarkStore.cpp compiles
// against the real logging call sites without an SD card or a HalStorage-backed log file.
#include <cstdarg>

struct SdDebugLog {
  static void log(const char*, const char*, ...) {}
};
