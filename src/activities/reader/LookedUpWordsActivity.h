#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "activities/ListTouchTarget.h"
#include "util/ButtonNavigator.h"
#include "util/DictionaryLookupController.h"
#include "util/LookupHistory.h"

class LookedUpWordsActivity final : public Activity {
 public:
  explicit LookedUpWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookCachePath)
      : Activity("LookedUpWords", renderer, mappedInput),
        cachePath(std::move(bookCachePath)),
        controller(renderer, mappedInput, *this, cachePath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string cachePath;

  // Clears any session dictionary override (set by long-press Confirm on the definition
  // screen) when this activity is destroyed, so a switch cannot outlive the screen that
  // hosted it. RAII rather than a call in onExit(): activities are heap-allocated and
  // deleted on exit, so the destructor always runs.
  Dictionary::SessionOverrideScope dictOverrideScope_;
  // History is paged from SD, not materialized: only the on-screen window lives
  // in RAM, so the list is bounded regardless of how large the history grows
  // (the prerequisite that makes the "Unlimited" cap safe). totalCount drives
  // the header/nav bounds; window[] holds one page, refilled on scroll/change.
  int totalCount = 0;
  static constexpr int WINDOW_CAP = 40;  // >= max rows per page; bounded RAM
  LookupHistory::Entry window[WINDOW_CAP];
  int windowStart = -1;  // newest-first index of window[0]; -1 = invalid
  int windowLen = 0;
  int selectedIndex = 0;
  bool deleteConfirmMode = false;
  bool confirmReleaseConsumed = false;
  int pagesUntilFullRefresh = 0;  // landscape ghost-scrub cadence counter (0 = scrub next render)

  DictionaryLookupController controller;
  ButtonNavigator buttonNavigator;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  // Convert UI index (0 = most recent) to 0-based file index (0 = oldest).
  int fileIndexOf(int uiIndex) const { return totalCount - 1 - uiIndex; }

  // Push the framebuffer with the list refresh policy (fast in portrait; fast +
  // periodic HALF scrub in landscape to clear differential ghosting snappily).
  void displayList();

  // Refresh totalCount from SD and invalidate the cached window.
  void refreshCount();
  // Fetch entry at a newest-first UI index, paging the window in if needed.
  // Returns nullptr only if the index is out of range.
  const LookupHistory::Entry* entryAt(int uiIndex);

  static const char* glyphFor(LookupHistory::Status s);

  // Rows the last render drew, so a tap can pick one (see ListTouchTarget).
  ListTouchTarget listTouch_;
};
