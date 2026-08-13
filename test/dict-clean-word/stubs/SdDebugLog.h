#pragma once
// Host-test stub for lib/SdDebugLog/SdDebugLog.h (device-only — writes to the SD
// card). Dictionary::locate logs the widened-scan retry here so a dictionary whose
// sort order disagrees with cistrcmp is visible in the on-device log; on the host
// these are no-ops.
namespace SdDebugLog {
inline void log(const char*, const char*, ...) {}
}  // namespace SdDebugLog
