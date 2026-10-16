#pragma once

#include <SdSystemDir.h>

#include <string>

#include "activities/ActivityManager.h"

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session. The live frontlight
// state rides along in the same RTC flag so the reboot is invisible: the light
// comes back exactly as it was, regardless of the Restore Light on Wake
// preference.

// home screen, with the selector on homeItem
void silentRestart(HomeMenuItem homeItem = HomeMenuItem::NONE);
void silentRestartToReader();                // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToSettings(int category);  // settings list at the given category index (0-3)
// Reboot into the File Transfer > Join Network flow on a pristine heap, so the
// WiFi + TLS working set has the contiguous RAM it needs on tight boards. A
// no-op on touch boards; the caller then proceeds without a reboot.
void silentRestartToJoinNetwork();

// Straight into the Translation activity on a fresh heap, re-reading the page text the
// activity stashed at translateStashPath(). Used when the WiFi/TLS heap gates fail on a
// fragmented heap. A missing or empty stash at boot falls back to the reader.
void silentRestartToTranslation();
// One page of source text, written by EpubReaderTranslationActivity before
// silentRestartToTranslation() and consumed (read + deleted) by setup(). On SD, not RTC:
// a page of CJK text does not fit the RTC slow memory that is left.
inline std::string translateStashPath() { return sdsystem::path("translate_pending.txt"); }

// Reboots immediately after an activity releases exclusive raw storage (USB Drive). The
// RTC target lands setup() on Home rather than resuming a reader whose cache the host may
// have changed underneath it. No frontlight capture: the USB-OTG handoff below resets the
// peripheral, and this path is not the invisible heap-defrag reboot.
void restartToHomeAfterStorageHandoff();
