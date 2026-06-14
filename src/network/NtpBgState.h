#pragma once

// Cross-task flags for the boot-time background NTP sync task (see
// maybeStartBackgroundNtpSync() in main.cpp). The task brings up the WiFi stack,
// which costs ~37KB and fragments the heap for the duration of the sync. Opening
// an EPUB during that window starves the 32KB contiguous inflate window and the
// renderer, which OOM-aborts the device (no exceptions on ESP32-C3). The reader
// open path checks `active` and, if set, waits/cancels before loading a book.
//
// Single-writer-each, so plain `volatile` is the correct primitive (no mutex):
//   - `active`: written only by the ntp_bg task (true for its whole WiFi
//     lifetime, false once WiFi is torn down or the task exits early).
//   - `cancel`: written only by the reader-open path to request early teardown;
//     polled by the task (connect loop + HalClock::syncFromNTP abort flag).
namespace NtpBg {
extern volatile bool active;
extern volatile bool cancel;
}  // namespace NtpBg
