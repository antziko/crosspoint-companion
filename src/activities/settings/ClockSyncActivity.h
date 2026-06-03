#pragma once

#include "activities/Activity.h"

// Manual NTP resync action. Presents the WiFi selection list first so the user
// can connect, then runs a forced sync (bypassing the once-per-device debounce),
// reports success/failure, and waits for Back.
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
  enum State { PICKING_WIFI, SYNCING, SUCCESS, NO_WIFI, FAILED };
  State state = PICKING_WIFI;
  char syncedTime[16] = {0};

  void onWifiSelectionComplete(bool success);
  void runSync();
};
