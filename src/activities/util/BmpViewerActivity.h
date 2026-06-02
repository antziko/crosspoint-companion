#pragma once

#include <functional>
#include <string>

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
  void loadSiblingImages();
  void renderImage();
  void doSetSleepCover();
  void doClearSleepCover();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  // True while a sleep cover (/sleep.bmp) exists: the Confirm button then clears
  // it instead of setting one. Refreshed in onEnter().
  bool coverExists = false;
};