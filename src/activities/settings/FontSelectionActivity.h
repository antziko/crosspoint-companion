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

  // Index in fonts_ matching the current selection (built-in family or SD name).
  int currentSelectionIndex() const;
};
