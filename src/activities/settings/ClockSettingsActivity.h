#pragma once

#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Top-level Clock settings submenu (system tab → Clock).
// Centralises everything clock-related: hardware mode (Off/RTC/RAM), display
// format, UTC offset, auto-sync behavior, home-screen entry visibility, and the
// "Sync clock now" action. The status bar only knows whether the clock is
// enabled — all configuration lives here.
class ClockSettingsActivity final : public Activity {
 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClockSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  // Visible menu items, derived from the current clock mode. ITEM_AUTO_SYNC only appears
  // for RTC mode — RAM mode loses its state on every boot, so a "sync on boot" toggle is
  // meaningless there. Rebuilt on entry and after any selection that could change mode.
  std::vector<uint8_t> visibleItems;

  void handleSelection();
  void rebuildVisibleItems();
};
