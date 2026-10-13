#pragma once
#include "SleepReviewFrame.h"
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  // `frameOut`, when set, receives the planes as they are drawn (see SleepReviewFrame).
  void renderBitmapSleepScreen(const Bitmap& bitmap, SleepReviewFrame::Writer* frameOut = nullptr) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void paintSleepFrame() const;

  bool fromTimeout = false;
};
