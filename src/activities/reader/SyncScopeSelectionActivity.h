#pragma once

#include "SyncScope.h"
#include "activities/Activity.h"
#include "activities/ListTouchTarget.h"
#include "util/ButtonNavigator.h"

/**
 * SyncScopeSelectionActivity lets the user choose what a KOReader sync run covers:
 * - "Everything"  - full sync (progress + bookmarks + stats + dictionary)
 * - "Progress"    - reading progress only
 * - "Bookmarks"   - bookmarks only
 * - "Stats"       - reading-time stats only
 * - "Dictionary"  - dictionary lookup history only
 *
 * On Confirm it returns a SyncScopeResult; on Back it returns a cancelled result.
 * The caller (EpubReaderActivity) launches the matching KOReaderSyncActivity scope.
 */
class SyncScopeSelectionActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

 public:
  explicit SyncScopeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SyncScopeSelection", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Rows the last render drew, so a tap can pick one (see ListTouchTarget).
  ListTouchTarget listTouch_;
};
