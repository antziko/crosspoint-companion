#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Dictionary/highlight word-select marker placement configuration. Groups the dwell-based
// marker settings (enable + two dwell thresholds) into their own sub-screen so the main Reader
// settings list stays short. These settings read/write CrossPointSettings directly and are not
// part of the shared getSettingsList() registry (device-only; not exposed on the web settings page).
class DictMarkerSettingsActivity final : public Activity {
 public:
  explicit DictMarkerSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DictMarkerSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void handleSelection();
};
