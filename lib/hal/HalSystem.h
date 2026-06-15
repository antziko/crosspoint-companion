#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

// Flush pending operator-new OOM events to /oom_trace.txt. Safe to call from the
// main loop (does SD I/O); cheap no-op when nothing is pending or in non-trace
// builds. Lets silent nothrow OOM near-misses be inspected untethered later.
void drainOomTrace();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();
}  // namespace HalSystem
