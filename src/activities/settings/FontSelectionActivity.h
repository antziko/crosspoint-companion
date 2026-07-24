#pragma once

#include <SdCardFontRegistry.h>

#include <string>

#include "FontComparePane.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Per-book / global font picker built on the shared FontComparePane (two-pane live compare +
// pinnable list). This host returns the chosen font via a FontSelectionResult; the caller applies
// it (per-book override or global settings). The unified Text Settings Font tab hosts the same pane
// with a live-apply callback instead — see TextSettingsActivity.
class FontSelectionActivity final : public Activity {
 public:
  // `currentBuiltinFamily` / `currentSdFamilyName` describe the selection to highlight on entry.
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const SdCardFontRegistry* registry, uint8_t currentBuiltinFamily,
                                 std::string currentSdFamilyName);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Build a FontSelectionResult from the pane's highlighted font and finish.
  void handleSelection();

  const SdCardFontRegistry* registry_;
  uint8_t currentBuiltinFamily_;
  std::string currentSdFamilyName_;
  ButtonNavigator buttonNavigator_;
  FontComparePane pane_;

  // Confirm handling: a tap commits the highlighted font; a hold pins/unpins it. The hold fires once
  // mid-press; the matching release must then NOT also commit.
  bool confirmArmed_ = false;
  bool pinFiredThisHold_ = false;

  // Layout dims cached in onEnter() so loop() (page sizing) and render() agree without recompute.
  int afterHeader_ = 0;
  int panesHeight_ = 0;  // combined height of the two stacked compare panes
  int usableHeight_ = 0;
};
