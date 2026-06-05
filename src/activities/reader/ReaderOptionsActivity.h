#pragma once
#include <I18n.h>

#include <string>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// In-reader per-book reader settings editor.
// Displays 6 settings items (font family, font size, line spacing, paragraph
// alignment, hyphenation, extra paragraph spacing).  Changes are applied live
// to the SETTINGS override and persisted to reader_settings.bin immediately.
class ReaderOptionsActivity final : public Activity {
 public:
  explicit ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string bookCachePath,
                                 const CrossPointSettings::ReaderOverride& initialOverride);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int ITEM_COUNT = 6;

  std::string cachePath;
  CrossPointSettings::ReaderOverride localOverride;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  // Cycle the current item to its next value and persist.
  void cycleCurrentItem();

  // Launch the full font picker (built-in + SD) for the font-family item and
  // apply the choice to this book's override.
  void openFontFamilyPicker();

  // Write localOverride to reader_settings.bin and re-apply to SETTINGS.
  void persistAndApply();

  // Display name and value string for each item index.
  static const char* getItemName(int index);
  std::string getItemValue(int index) const;
};
