#pragma once
// Host-test stub for lib/SdDebugLog/SdDebugLog.h (device-only — writes to the SD card).
// ChapterHtmlSlimParser logs soft-flush points, image placement and layout OOM here so a
// pagination problem is visible in the on-device log; on the host these are no-ops.
namespace SdDebugLog {
inline void log(const char*, const char*, ...) {}
}  // namespace SdDebugLog
