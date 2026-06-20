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
  // showMinSession controls the "min read time for stats" item. Readers without
  // reading-time tracking (e.g. the plain-text reader) pass false to hide it.
  explicit ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookCachePath,
                                 const CrossPointSettings::ReaderOverride& initialOverride, bool showMinSession = true);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // MIN_SESSION is the last item; hiding it just trims the count by one.
  static constexpr int ITEM_COUNT = 7;
  int itemCount() const { return showMinSession ? ITEM_COUNT : ITEM_COUNT - 1; }

  std::string cachePath;
  CrossPointSettings::ReaderOverride localOverride;
  bool showMinSession = true;
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
