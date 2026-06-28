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
  // Resolve the actual fontId for a list entry WITHOUT mutating SETTINGS, so the
  // preview pane can render a font the user has not committed to. Returns 0 if
  // unavailable (e.g. an SD font not loaded), in which case the pane shows only
  // the label.
  int getFontIdForPreview(int index) const;
  void renderPreviewPane(int top, int height, int fontId, const char* fontName) const;

  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;  // index used by valueSetter
  };

  const SdCardFontRegistry* registry_;
  uint8_t currentBuiltinFamily_;
  std::string currentSdFamilyName_;
  ButtonNavigator buttonNavigator_;
  std::vector<FontEntry> fonts_;
  int selectedIndex_ = 0;
  // Two-step confirm: first Confirm previews the highlighted font (sets this to
  // selectedIndex_), second Confirm (when they match) commits it. Preview never
  // touches SETTINGS — the chosen font is still returned via FontSelectionResult.
  int previewFontIndex_ = 0;

  // Layout dims cached in onEnter() so loop() and render() agree without recompute.
  int afterHeader_ = 0;
  int bottomReserved_ = 0;
  int usableHeight_ = 0;
  int previewHeight_ = 0;

  // Index in fonts_ matching the current selection (built-in family or SD name).
  int currentSelectionIndex() const;
};
