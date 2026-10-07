#pragma once
// Host-test stub for lib/SdDebugLog/SdDebugLog.h (device-only — writes to the SD
// card). The code under test uses SdDebugLog::log only for X3-readable diagnostics; on
// the host these are no-ops.
namespace SdDebugLog {
inline void log(const char*, const char*, ...) {}
}  // namespace SdDebugLog
