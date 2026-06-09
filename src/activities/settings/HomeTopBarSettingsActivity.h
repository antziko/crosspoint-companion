#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Home screen top bar clock/date configuration (independent from reader status bar)
class HomeTopBarSettingsActivity final : public Activity {
 public:
  explicit HomeTopBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HomeTopBarSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void handleSelection();
};
