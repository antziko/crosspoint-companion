#pragma once

#include <SdCardFontRegistry.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "FontComparePane.h"
#include "TextSettingsPreview.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"

// Reader text settings with a shared live preview pane: tab bar
// (Font | Size | Layout | Style) is position 0 of the Up/Down nav ring, same
// idiom as SettingsActivity. Family/Size rows apply on Confirm; Layout/Style
// rows toggle or open an OptionPopup picker. (Tab::Family/Style are the enum
// names for the Font/Style tabs.)
//
// LOCAL(feat): the Font tab is NOT a FreeInkUI list. It is backed by
// FontComparePane — the two-pane committed-vs-highlighted compare plus a
// pinnable font list — which draws itself with the legacy renderer into pixel
// regions. Everything else on this screen is FreeInkUI. Mixing is the
// established idiom here rather than a workaround: render() already draws the
// header, the shared preview, the caption and the button hints with the legacy
// renderer around renderUi(). The Font tab simply extends that to the body
// region. Consequences, all handled below: the Font tab registers no FUI list,
// so its taps and swipes are routed by handleFamilyTouch() instead of the
// base's routeListTouch(); its visibleRows is computed here rather than by
// syncTabListViewport(); and navigation honours the pane's nav-lock.
class TextSettingsActivity final : public UiTabListActivity {
 public:
  enum class Tab : uint8_t { Family, Size, Layout, Style, Count };
  // Where edits land. Settings edits the global defaults with every tab. The Book modes are
  // opened from the reader's Text panel and edit that book's override, written to
  // `bookCachePath`; settings a book does not store (word/character spacing, embedded style,
  // anti-aliasing) stay reader-wide. BookFont shows only the font list; BookMore shows only
  // the Layout/Style rows the Text panel does not already carry.
  enum class Mode : uint8_t { Settings, BookFont, BookMore };

  TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                       Tab initialTab = Tab::Family, Mode mode = Mode::Settings, std::string bookCachePath = "");

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  void drawChrome() override;
  void drawFooter() override;

  // Row indices per tab. enum class (not plain enum) so a LayoutRow can't be
  // silently confused with a StyleRow of equal value.
  enum class LayoutRow {
    LineSpacing,
    WordSpacing,
    CharacterSpacing,
    ParaSpacing,
    ParaIndent,
    Alignment,
    ScreenMargin,
    Count
  };
  enum class StyleRow { FocusReading, Hyphenation, EmbeddedStyle, AntiAliasing, Count };

  // --- UiTabListActivity contract ---
  int listCount() const override;
  int tabCount() const override { return visibleTabCount_; }
  int activeTab() const override { return tabIndex(tab_); }
  const char* tabLabel(int index) const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override { switchTab(direction); }
  bool handleButtons() override;
  bool handleCustomInput() override;
  // Ring walk, plus the Font tab's nav-lock gate and highlight mirroring.
  void navigateButtons() override;

  // LOCAL(feat): applies the compare pane's highlighted font.
  void applyFamily();
  void applySize(int listIndex);
  // Repopulates sizes_ (and currentSizeIndex_) from the active family's
  // installed point sizes. Call after any family change.
  void rebuildSizeList();
  void confirmLayoutRow(int row);
  void confirmStyleRow(int row);
  // Applies the row at the given list index for the active tab (Confirm and tap share this).
  void activateRow(int row);

  std::string layoutValueText(int row) const;
  std::string styleValueText(int row) const;
  // List index -> row enum value for the active mode (book modes hide some rows).
  int layoutRowAt(int index) const { return layoutRows_[index]; }
  int styleRowAt(int index) const { return styleRows_[index]; }
  // Position of `tab` in the visible tab list, or -1 when this mode hides it.
  int tabIndex(Tab tab) const;
  bool bookMode() const { return mode_ != Mode::Settings; }
  // Makes the edited book override live (memory only) so the preview and font loader see it.
  void applyBook();
  // Saves the routed fields: the book's override file in a book mode, else the global settings.
  void persist();
  static bool styleRowIsToggle(int row);
  bool styleRowChecked(int row) const;
  // Button-hint label for Confirm at the current ring position.
  const char* confirmLabelText() const;
  // True when the focused list row is a setting the preview cannot reflect.
  bool focusedRowHasNoPreview() const;
  void switchTab(int direction = 1);

  // --- Font tab (compare pane) ---

  bool onFamilyTab() const { return tab_ == Tab::Family; }
  // Vertical layout of the preview / tab-bar / pane-list regions. Only the Font
  // tab needs it: the other tabs get their body rect from the FUI content
  // margin. Kept in one place so render() and touch agree on the list region.
  struct PaneGeometry {
    int previewTop;
    int tabTop;
    int listTop;
    int listHeight;
  };
  PaneGeometry paneGeometry() const;
  // Mirror the ring position (0 = tab bar, 1..N = row) onto the pane's highlight,
  // arming its nav-lock so input cannot outrun the SD preview load.
  void syncFamilyPaneHighlight();
  // Tap/swipe routing for the Font tab, which has no FUI list to route through.
  bool handleFamilyTouch();

  // Row storage for the active tab: rowItems_ (label/actionValue) is rebuilt
  // only when the tab or its backing data changes (rebuildRowItems(), called
  // from onEnter()/onTabAction()/switchTab()); rowValues_ holds the live
  // per-row value text, refreshed every buildScreen() call by assigning into
  // the existing strings (no vector growth), so steady-state rendering never
  // allocates/frees row storage. Empty on the Font tab, which has no FUI list.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  struct SizeEntry {
    std::string name;  // the point size, rendered for display ("14 pt")
    uint8_t pointSize;
  };

  const SdCardFontRegistry* registry_;
  OptionPopup optionPopup_;
  // LOCAL(feat): the Font tab is backed by the shared two-pane compare + pinnable list.
  // It owns the font list (built-in + SD, pinned-first) and the committed/highlighted state,
  // so this screen keeps no fonts_ vector or currentFamilyIndex_ of its own.
  FontComparePane fontPane_;
  std::vector<SizeEntry> sizes_;
  textsettings::PreviewLayout previewLayout_;  // cached preview line layout; relaid only on setting/geometry change

  Tab tab_;
  Mode mode_;
  std::string bookCachePath_;
  CrossPointSettings::ReaderOverride book_;
  // The fields a book can override, pointing into book_ in a book mode and at the globals otherwise.
  struct Fields {
    uint8_t* fontFamily;
    uint8_t* fontPointSize;
    uint8_t* lineSpacing;
    uint8_t* paragraphAlignment;
    uint8_t* paragraphSpacing;
    uint8_t* paragraphIndent;
    uint8_t* screenMargin;
    uint8_t* hyphenationEnabled;
    char* sdFontFamilyName;  // char[32] in both
  } f_{};
  std::array<Tab, static_cast<size_t>(Tab::Count)> visibleTabs_{};
  int visibleTabCount_ = 0;
  std::array<uint8_t, static_cast<size_t>(LayoutRow::Count)> layoutRows_{};
  int layoutRowCount_ = 0;
  std::array<uint8_t, 4> styleRows_{};
  int styleRowCount_ = 0;
  int currentSizeIndex_ = 0;
  // LOCAL(feat): Font tab Confirm = tap-commits / hold-pins (mirrors FontSelectionActivity).
  // confirmArmed_ also serves as the Confirm press-origin latch for every tab: this screen
  // opens from SettingsActivity on Confirm PRESS, so the release ending that click lands
  // here and must not be mistaken for input of our own.
  bool confirmArmed_ = false;
  bool pinFiredThisHold_ = false;
  bool backPressActive_ = false;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  // Horizontal extent of the hint-safe area (a side column of hints in landscape).
  int safeX = 0;
  int safeWidth = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
