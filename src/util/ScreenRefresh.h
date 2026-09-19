#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SdDebugLog.h>

#include "CrossPointSettings.h"
#include "activities/RenderLock.h"

// The "Refresh Screen" action, in one place.
//
// Whole-page ghost clear, driven from a blanked framebuffer so it pushes the WHOLE panel
// rather than just changed pixels. Two entry points run it -- the power button's short press
// (main.cpp) and the reader's Confirm/Home hold -- and they must not drift, so neither owns
// the body.
//
// The mode is the user's choice in Settings > Display > Refresh Screen Mode, and the panel
// tradeoff is real:
//   - FAST: grayscale-safe. A HALF/FULL clear firms the e-ink particles too hard for the X4
//     grayscale LUT to darken back, washing AA/image/sleep pages whitish. FAST avoids that
//     (same trick as the image-blanking dance).
//   - HALF: stronger ghost clear. Safe on X3 (1-bit panel, no grayscale image pass); on X4 may
//     wash grayscale content whitish.
//   - FULL: multi-cycle deep clean (deepCleanPanel), for image sticking that a single inversion
//     cannot release -- a black rule held at a fixed y for minutes, e.g. the themed header
//     underline through an SD firmware write. ~15 s of visible black/white flashing, which is
//     why it is a deliberate user action and not a default.
//
// Takes the render lock itself, so the CALLER MUST NOT HOLD IT (the lock is not recursive and a
// second take from the same task hangs rather than crashes). Both call sites reach this from
// their input handler, which does not hold it.
//
// Every branch leaves the framebuffer blank -- deepCleanPanel by contract (GfxRenderer.h), the
// other via clearScreen() -- so the caller must re-render afterwards, and any activity holding
// differential state has to drop it first or that render restores pixels this just wiped.
inline void refreshScreenNow(const GfxRenderer& renderer) {
  if (SETTINGS.refreshScreenMode == CrossPointSettings::RSM_FULL) {
    unsigned long cleanMs = 0;
    {
      RenderLock lock;
      cleanMs = renderer.deepCleanPanel();
    }
    // On the SD log, not just serial: this is the one burn-in remedy the user can trigger by
    // hand, and without a trace here a later "the line came back" report can't be told apart
    // from "no clean was ever run". Force-enabled for the same reason.
    SdDebugLog::setEnabled(true);
    SdDebugLog::log("GFX", "deepclean manual cycles=3 ms=%lu", cleanMs);
    return;
  }

  const HalDisplay::RefreshMode clearMode =
      SETTINGS.refreshScreenMode == CrossPointSettings::RSM_HALF ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  RenderLock lock;
  // panelWhiteFill(), not a plain clearScreen(). The framebuffer is always normal polarity and
  // the driver inverts on the way to the panel (FreeInkDisplay.cpp:601), so in NIGHT MODE the
  // default 0xFF drives the glass to BLACK -- from a page that is already ~90% black. With Fast
  // (the default mode) that is a differential from a black page to a black panel: only the text
  // pixels move, so the clear writes the outgoing page's shape as a charge differential instead
  // of erasing it, and it surfaces as a ghost of that page minutes later. With Half it is a
  // full-panel shove further toward black with nothing bringing it back.
  //
  // Filling for panel-white instead gives ~90% of the pixels a real swing off black, and the
  // repaint that follows returns them -- so the clear is a black->white->black round trip in
  // night mode just as it is white->black->white in day mode. deepCleanPanel (the Full mode
  // above) already alternates both grounds itself, so it needs no equivalent.
  renderer.clearScreen(renderer.panelWhiteFill());
  renderer.displayBuffer(clearMode);
}
