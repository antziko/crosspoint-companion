#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity showing the list of configured KOReader sync servers.
 * Tap a server to open its editor. Hold-select to duplicate.
 * The active/default server is marked with a filled-circle prefix.
 */
class KOReaderServerListActivity final : public Activity {
 public:
  explicit KOReaderServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KOReaderServerList", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool longPressFired = false;      // Swallow Confirm release after a hold-duplicate fires
  bool confirmPressActive = false;  // True only when a Confirm press originated inside this activity

  int getItemCount() const;
  void handleSelection();
  void duplicateSelectedServer();
};
