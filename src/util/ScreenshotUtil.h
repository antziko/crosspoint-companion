#pragma once
#include <GfxRenderer.h>

#include <cstddef>

#include "ScreenshotInfo.h"

class MappedInputManager;

class ScreenshotUtil {
 public:
  static void takeScreenshot(GfxRenderer& renderer);
  // Modal "Take screenshot? Save / Cancel" prompt for the global button combo, so a
  // mistaken press doesn't silently save. Overlays a popup, waits for the trigger
  // combo to release, then returns true on Confirm / false on Cancel or timeout,
  // restoring the underlying screen either way. Returns true without prompting if the
  // screen can't be stored/restored (no scratch buffer) — falls back to old behavior.
  static bool confirmScreenshot(GfxRenderer& renderer, MappedInputManager& input);
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height);

 private:
  static void buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize);
};
