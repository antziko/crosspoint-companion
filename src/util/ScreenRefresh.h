#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SdDebugLog.h>

#include "CrossPointSettings.h"
#include "activities/RenderLock.h"

// The "Refresh Screen" action, in one place.
//
// Whole-page ghost clear. Two entry points run it -- the power button's short press
// (main.cpp) and the Confirm/Home hold (the reader's own case, plus main.cpp's global one for
// every other screen) -- and they must not drift, so neither owns the body. The control
// centre's refresh tile is the third and does the same thing inline, promoting the repaint.
//
// The mode is the user's choice in Settings > Display > Refresh Screen Mode:
//   - HALF (default): the charge SCRUB. Seeds the panel's OLD plane with the COMPLEMENT of the
//     target, so every pixel is forced through a transition cell and none idles with stale
//     charge (Uc8279X4Driver.cpp:410-419). Handed to the REPAINT via promoteNextRefresh()
//     rather than pushed as a blank frame -- see below, this is the whole point.
//   - FAST: differential, so it cannot clear a ghost; kept as the do-the-least option and as
//     the escape hatch on grayscale pages, where a GC waveform firms the particles too hard
//     for the X4 grayscale LUT to darken back and washes AA/image pages whitish. Irrelevant
//     under night mode, which disables the grayscale path outright (FreeInkDisplay.cpp:811).
//   - FULL: multi-cycle deep clean (deepCleanPanel), for image sticking that a single pass
//     cannot release -- a black rule held at a fixed y for minutes. ~15 s of visible flashing,
//     which is why it is a deliberate user action and not a default.
//
// WHY HALF PROMOTES INSTEAD OF CLEARING. Pushing a blank frame and letting the activity
// repaint is two passes, and the second one -- the frame the panel actually parks on -- runs
// whatever mode the activity asked for, which is FAST everywhere. So the scrub was spent on a
// frame that was immediately overwritten and the held image was laid down by a DU partial,
// which never develops full black. Promoting puts the GC waveform on the content frame, in one
// pass. Under night mode that is the difference between a DU black that keeps relaxing (and
// lets the previous page surface as it lightens) and a GC black that holds.
//
// Never FULL for a single pass: FULL seeds OLD = white, so it drives only the black-target
// pixels and leaves everything else parked in the no-transition cell. Under night mode the
// stranded population is the white text -- the page shape, written as stale charge. That is
// why deepCleanPanel uses FULL for its black phase and HALF for its white one
// (GfxRenderer.cpp:2074-2092), and it is the rule any new clear has to follow.
//
// Takes the render lock on the branches that paint, so the CALLER MUST NOT HOLD IT (the lock is
// not recursive and a second take from the same task hangs rather than crashes). Both call
// sites reach this from their input handler, which does not hold it.
//
// The FAST and FULL branches leave the framebuffer blank, so the caller must re-render and any
// activity holding differential state has to drop it first. HALF leaves the framebuffer alone
// -- the complement seed is what makes the scrub complete, not a blanked buffer -- but the
// caller repaints either way, so the contract is unchanged from outside.
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

  if (SETTINGS.refreshScreenMode == CrossPointSettings::RSM_HALF) {
    // Traced by GfxRenderer::displayBuffer when the promotion is consumed, which also records
    // whether the waveform actually ran -- the request on its own proves nothing.
    SdDebugLog::setEnabled(true);
    renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH, "manual");
    return;
  }

  // FAST keeps the clear-and-push: promoting FAST onto a repaint that is already FAST would
  // make the action do nothing at all. panelWhiteFill(), not a plain clearScreen() -- the
  // framebuffer is always normal polarity and the driver inverts on the way to the panel
  // (FreeInkDisplay.cpp:601), so the default 0xFF would drive the glass to BLACK in night mode,
  // from a page that is already ~90% black.
  RenderLock lock;
  renderer.clearScreen(renderer.panelWhiteFill());
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
