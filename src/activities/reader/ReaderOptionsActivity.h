#pragma once
#include <I18n.h>

#include <string>

#include "../settings/FontComparePane.h"
#include "../settings/TextSettingsPreview.h"
#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// In-reader per-book reader settings editor.
// Displays the per-book text settings (font family, font size, line spacing,
// paragraph alignment, hyphenation, extra paragraph spacing, screen margin) plus
// the word-select button assignment and the stats min-read-time.  Changes are
// applied live to the SETTINGS override and persisted to reader_settings.bin
// immediately.
class ReaderOptionsActivity final : public Activity {
 public:
  // showMinSession controls the "min read time for stats" item. Readers without
  // reading-time tracking (e.g. the plain-text reader) pass false to hide it.
  // sampleText seeds the live preview with the book's own text (the reader passes its
  // current page); empty falls back to the built-in pangram.
  explicit ReaderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookCachePath,
                                 const CrossPointSettings::ReaderOverride& initialOverride, bool showMinSession = true,
                                 std::string sampleText = "");

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // MIN_SESSION is the last item; hiding it just trims the count by one.
  static constexpr int ITEM_COUNT = 9;
  int itemCount() const { return showMinSession ? ITEM_COUNT : ITEM_COUNT - 1; }

  std::string cachePath;
  CrossPointSettings::ReaderOverride localOverride;
  bool showMinSession = true;
  // Book text sampled into the preview pane; empty => pangram fallback.
  std::string sampleText;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  // Cycle the current item to its next value and persist.
  void cycleCurrentItem();

  // Embedded font-family picker (no separate screen): the font list replaces the settings
  // list while open, and the preview above shows the highlighted font against the book text.
  void openInlineFontList();
  // Live-apply the highlighted font to localOverride so the preview renders in it.
  void applyHighlightedFont();
  // Persist the highlighted font and close the list.
  void commitInlineFont();
  // Restore the pre-picker font (and resident SD font) and close the list.
  void cancelInlineFont();

  // Write localOverride to reader_settings.bin and re-apply to SETTINGS.
  void persistAndApply();

  // Display name and value string for each item index.
  static const char* getItemName(int index);
  std::string getItemValue(int index) const;

  // Live sample-text preview shown above the list; reflects the per-book override
  // live via the reader engine (same shared component as the global Text Settings).
  // Cached layout, relaid only when a layout-affecting value or the geometry changes.
  textsettings::PreviewLayout previewLayout_;

  // False means the framebuffer already holds a valid header/preview/hints frame, so render()
  // repaints only the list band. Set wherever anything outside that band can change; see the
  // comment in render() for the framebuffer-ownership assumption this rests on.
  bool fullRedraw_ = true;

  // Embedded font-family picker state. fontPane_ owns the font list + SD-preview loading;
  // the two-pane compare it can draw is unused here — we render a single book-text preview.
  FontComparePane fontPane_;
  bool fontListOpen_ = false;
  // Arm Confirm only on a press seen while the list is open, so the release of the press
  // that opened it (Confirm on the Font Family row) does not immediately commit.
  bool fontConfirmArmed_ = false;
  // Font override saved on open, restored if the picker is cancelled (Back).
  uint8_t savedFontFamily_ = 0;
  char savedSdFontFamilyName_[sizeof(CrossPointSettings::ReaderOverride::sdFontFamilyName)] = "";
};
