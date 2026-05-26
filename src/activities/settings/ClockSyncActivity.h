#pragma once

#include "activities/Activity.h"

// Manual NTP resync action. Runs a forced sync (bypassing the once-per-device debounce),
// reports success/failure, then waits for Back. Requires WiFi to already be connected.
class ClockSyncActivity final : public Activity {
 public:
  explicit ClockSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClockSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  // CONFIRM_RTC: device has an RTC. Show its current time and prompt the user to refresh
  // from NTP (Confirm = Sync, Back = Cancel). RAM-mode / no-RTC devices skip this state
  // entirely since they have no existing time to display.
  enum State { CONFIRM_RTC, SYNCING, SUCCESS, NO_WIFI, FAILED };
  State state = SYNCING;
  char displayedTime[16] = {0};
  // True after we've launched WifiSelectionActivity once. Prevents a relaunch loop if the
  // user backs out of WiFi selection.
  bool wifiLaunchAttempted = false;

  void runSync();
};
