#pragma once

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

// Reboots immediately after an activity releases exclusive raw storage (USB Drive). The
// RTC target lands setup() on Home rather than resuming a reader whose cache the host may
// have changed underneath it. No frontlight capture: the USB-OTG handoff below resets the
// peripheral, and this path is not the invisible heap-defrag reboot.
void restartToHomeAfterStorageHandoff();
