#pragma once
#include <string>

#include "activities/Activity.h"
#include "activities/TouchActionBar.h"

// One-shot on-wake prompt: shows the wallpaper that was displayed entering the last
// sleep and lets the user Keep, Remove, or Skip it before landing on Home/Reader.
//   Keep   -> rename to "<base>.keep.bmp" (stays in rotation, never prompted again)
//   Remove -> rename to ".<name>" (hidden, dropped from rotation, reversible)
//   Skip   -> leave unchanged (may be prompted again next time it is shown)
// Launched from main.cpp boot routing only when SETTINGS.reviewSleepImageOnWake is set
// and the recorded image still exists and is not already a kept image.
class SleepImageReviewActivity final : public Activity {
 public:
  SleepImageReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string imagePath,
                           bool resumeToReader, std::string readerPath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void renderImage();
  void doKeep();
  void doRemove();
  // Clear the recorded sleep image from state, then route on to the destination the
  // boot logic chose (reader resume or home).
  void finishToDestination();

  std::string imagePath;
  bool resumeToReader = false;
  std::string readerPath;

  // On-screen answers for touch boards, which draw no button hints (see
  // TouchActionBar): Skip / Keep / Remove.
  TouchActionBar actionBar_;
};
