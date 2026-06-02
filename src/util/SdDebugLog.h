#pragma once

// Lightweight on-SD debug log so failures can be inspected untethered (no serial
// monitor). Currently used to capture OPDS browser / HTTP fetch errors.
//
// Writes appended lines to /opds_debug.log on the SD card. Disabled by default;
// the OPDS browser enables it while active so other code paths don't spam it.
// The file is size-capped (rotated) so it can't grow without bound.
namespace SdDebugLog {

// Path of the log file on the SD card root.
inline constexpr const char* PATH = "/opds_debug.log";

// Enable/disable logging globally. When disabled, log() is a cheap no-op.
void setEnabled(bool enabled);
bool isEnabled();

// Delete the existing log (call when opening the browser to start a fresh trace).
void clear();

// Append one printf-style line, prefixed with millis() and tag. No-op if disabled.
void log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace SdDebugLog
