#pragma once
// Host stub: no shared inflate window, so SdCardFont takes its heap fallback.
#include <cstddef>
#include <cstdint>

struct InflateReader {
  static uint8_t* acquireScratch(size_t) { return nullptr; }
  static void releaseScratch() {}
};
