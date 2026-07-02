#pragma once

#include <SdCardFontRegistry.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FontSelectionActivity final : public Activity {
 public:
  // `currentBuiltinFamily` / `currentSdFamilyName` describe the selection to
  // highlight on entry; the chosen font is returned via a FontSelectionResult
  // (the caller applies it to global settings or a per-book override).
  explicit FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const SdCardFontRegistry* registry, uint8_t currentBuiltinFamily,
                                 std::string currentSdFamilyName);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void handleSelection();
  // Live preview: point the bottom pane at the highlighted font. render() does the
  // actual (SD) font loading, so this only re-targets and locks nav. Called per step.
  void previewSelected();
  // Resolve the fontId for a list entry WITHOUT mutating SETTINGS, loading its SD family
  // into the font manager first if needed so the pane can render it. Returns 0 if
  // unavailable. The manager holds ONE SD family at a time (loading the highlighted SD
  // font swaps out the user's selection; onExit() restores it). Built-ins are always
  // resident (no load, no swap).
  int loadPaneFontId(int index);
  // Draw one preview pane: wrapped sample text in `fontId`, with the font's name (`label`)
  // as a caption at the bottom so each pane is identifiable.
  void renderPreviewPane(int top, int height, int fontId, const char* label) const;
  // Toggle the pinned state of the highlighted font, persist, re-sort pinned-first,
  // and keep the highlight on the same font. Triggered by holding Confirm.
  void togglePinSelected();
  // Stable-sort fonts_ so pinned entries come first; preserves relative order.
  void applyPinSort();

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;  // index used by valueSetter
    std::string key;       // pin key: "@b<index>" (builtin) or "@s<familyName>" (SD)
    bool pinned = false;
  };

  const SdCardFontRegistry* registry_;
  uint8_t currentBuiltinFamily_;
  std::string currentSdFamilyName_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;
  // Font shown in the bottom (preview) pane. Tracks the highlight live; render() loads
  // it on demand. Never touches SETTINGS — the chosen font is returned via
  // FontSelectionResult only on commit.
  int previewFontIndex_ = 0;
  // Set once a preview has loaded a non-resident SD family into the font manager, so
  // onExit() knows to restore the user's actual selection.
  bool didLoadPreview_ = false;
  // Nav lock: a navigation step requests a preview render, then blocks further up/down
  // until render() completes. Stops held/rapid input from racing ahead of the (slow,
  // SD-loading) preview so the highlight never overshoots the pane.
  bool navLocked_ = false;
  // Set when a Confirm-hold pins a font, so the subsequent Confirm release does
  // NOT also commit/exit. Reset on release.
  bool pinFiredThisHold_ = false;
  // Only honor a Confirm release once a Confirm press has occurred *inside* this
  // activity. Without this, the release of the very press that opened Font Family
  // arrives on the first loop and instantly commits/exits.
  bool confirmArmed_ = false;

  // Layout dims cached in onEnter() so loop() and render() agree without recompute.
  int afterHeader_ = 0;
  int bottomReserved_ = 0;
  int usableHeight_ = 0;
  int previewHeight_ = 0;

  // Index in fonts_ matching the current selection (built-in family or SD name).
  int currentSelectionIndex() const;
};
