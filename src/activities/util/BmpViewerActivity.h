#pragma once

#include <functional>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Re-renders the current image. Invoked on every requestUpdate (e.g. the
  // power-button manual refresh), so a manual refresh actually redraws the
  // bitmap instead of leaving the cleared (white) framebuffer on screen.
  void render(RenderLock&&) override;

 private:
  // Find the immediate previous/next sibling .bmp of the current file by a single
  // bounded directory scan (largest name < current, smallest name > current). Holds O(1)
  // RAM regardless of how many images the folder has — a folder of thousands of sleep
  // BMPs would OOM-abort if every name were materialised into a vector.
  void computeSiblings();
  void renderImage();
  void doSetSleepCover();
  void doClearSleepCover();

  std::string filePath;
  // Immediate neighbours of the current image (filenames only, empty = none). Recomputed
  // each time the shown image changes, so navigation never holds the whole listing.
  std::string prevName;
  std::string nextName;
  // True while a sleep cover (/sleep.bmp) exists: the Confirm button then clears
  // it instead of setting one. Refreshed in onEnter().
  bool coverExists = false;
};