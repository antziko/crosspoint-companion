#pragma once

#include <cstdint>

// Lightweight on-SD debug log so failures can be inspected untethered (no serial
// monitor). Currently used to capture OPDS browser / HTTP fetch errors.
//
// Writes appended lines to /opds_debug.txt on the SD card. Disabled by default;
// the OPDS browser enables it while active so other code paths don't spam it.
// The file is size-capped (rotated) so it can't grow without bound.
namespace SdDebugLog {

// Path of the log file on the SD card root. .txt (not .log) so the on-device file
// browser opens it directly on both X3 and X4 without a cable.
inline constexpr const char* PATH = "/opds_debug.txt";

// Enable/disable logging for the current context (set by activities/network code
// while they are active). When disabled, log() is a cheap no-op.
void setEnabled(bool enabled);
bool isEnabled();

// Master kill switch, driven by the user setting (SETTINGS.sdCardLogging). log()
// writes only when BOTH the master switch and the per-context flag are enabled.
// Pushed in from src (lib cannot read CrossPointSettings — layering). Defaults to
// true so pre-settings-load boot logging and TRACE_HEAP builds still emit; once
// settings load it is set to the user's choice (default off).
void setMasterEnabled(bool enabled);

// Delete the existing log (call when opening the browser to start a fresh trace).
void clear();

// Append one printf-style line, prefixed with millis() and tag. No-op if disabled.
void log(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Heap fragmentation + radio snapshot, captured together because their
// correlation is the diagnostic signal: the X3 HTTPS-stall investigation found
// `internalLargest` (contiguous internal-SRAM, where WiFi RX buffers must come
// from — needs ~1.6KB blocks) cratering to ~2KB during slow transfers while
// `largest8Bit`/`heapFree` looked fine and RSSI stayed steady. See SUMMARY.md
// Part B Appendix. Both HTTP code paths (HttpDownloader's streaming GET and
// KOReaderSyncClient's perform()-based GET/PUT) log this shape so their traces
// are directly comparable.
struct NetSnapshot {
  uint32_t heapFree;
  uint32_t largest8Bit;
  uint32_t internalFree;
  uint32_t internalLargest;
  int8_t rssi;
};

NetSnapshot captureNetSnapshot();

}  // namespace SdDebugLog
