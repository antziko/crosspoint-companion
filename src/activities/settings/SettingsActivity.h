#pragma once
#include <I18n.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  RemapFrontButtonsCW,
  CustomiseStatusBar,
  CustomiseTopBar,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  PruneCache,
  RepaginateCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  // Open a nested settings sub-screen listing every setting in SettingInfo::subCategory.
  // Rendered by a category-scoped SettingsActivity instance — see SettingInfo::SubScreen.
  OpenSubCategory,
  TextSettings,  // LOCAL(feat-dictionary): #2605 Text Settings screen
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  std::vector<StrId> enumValues;
  std::vector<std::string> enumStringValues;  // runtime alternative to StrId enumValues (for SD card fonts etc.)
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)
  bool inTextSettings = false;           // Surfaced in the Text Settings screen; hidden from the flat Reader list

  // For OpenSubCategory actions: the category whose settings the nested sub-screen lists.
  StrId subCategory = StrId::STR_NONE_OPT;

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for the ~6 settings stored outside CrossPointSettings, e.g.
  // KOReaderCredentialStore, font-family, dictionary). Held out-of-line so the four
  // std::function objects (~64-96 B on this 32-bit target) don't bloat all ~59 entries — only
  // the few dynamic ones allocate a DynamicAccessors; the rest hold a null 8-byte pointer.
  // shared_ptr (not unique_ptr) because SettingInfo must stay copyable: getSettingsList() returns
  // the list by value, copying baseList. Copies happen only at boot/settings-open/web, never in
  // the render loop, so the atomic-refcount cost is negligible here.
  struct DynamicAccessors {
    std::function<uint8_t()> valueGetter;
    std::function<void(uint8_t)> valueSetter;
    std::function<std::string()> stringGetter;
    std::function<void(const std::string&)> stringSetter;
  };
  std::shared_ptr<DynamicAccessors> dyn;

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  SettingInfo& withTextSettings() {
    inTextSettings = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  // A row that opens a nested settings sub-screen listing every setting tagged `category`.
  // nameId is both the row label and the sub-screen title.
  static SettingInfo SubScreen(StrId nameId, StrId category) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = SettingAction::OpenSubCategory;
    s.subCategory = category;
    return s;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, std::function<uint8_t()> getter,
                                 std::function<void(uint8_t)> setter, const char* key = nullptr,
                                 StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.dyn = std::make_shared<DynamicAccessors>();
    s.dyn->valueGetter = std::move(getter);
    s.dyn->valueSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, std::function<std::string()> getter,
                                   std::function<void(const std::string&)> setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.dyn = std::make_shared<DynamicAccessors>();
    s.dyn->stringGetter = std::move(getter);
    s.dyn->stringSetter = std::move(setter);
    s.key = key;
    s.category = category;
    return s;
  }
};

class SettingsActivity final : public UiTabListActivity {
  int initialCategory = 0;        // Category to open on first onEnter()
  int selectedCategoryIndex = 0;  // Currently selected category
  int settingsCount = 0;

  // Sub-screen mode: when subCategory_ != STR_NONE_OPT this instance renders a single flat list
  // of every setting tagged with that category (no tab bar, no category switching, Back pops).
  // subTitle_ is the header/label. Default (STR_NONE_OPT) is the normal 4-tab top-level screen.
  StrId subCategory_ = StrId::STR_NONE_OPT;
  StrId subTitle_ = StrId::STR_NONE_OPT;
  bool isSubScreen() const { return subCategory_ != StrId::STR_NONE_OPT; }

  // Per-category settings derived from shared list + device-only actions
  std::vector<SettingInfo> displaySettings;
  std::vector<SettingInfo> readerSettings;
  std::vector<SettingInfo> controlsSettings;
  std::vector<SettingInfo> systemSettings;
  const std::vector<SettingInfo>* currentSettings = nullptr;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue) for *currentSettings, rebuilt only when
  // the active category or a category's setting list changes
  // (rebuildRowItems(), called from selectCategory()/rebuildSettingsLists())
  // — not on every repaint. rowValues_ holds the live per-row value text,
  // refreshed every buildScreen() call by assigning into the existing
  // strings (no vector growth).
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  static constexpr int categoryCount = 4;
  static const StrId categoryNames[categoryCount];

  // --- UiTabListActivity contract ---
  int listCount() const override { return settingsCount; }
  // LOCAL(feat): a sub-screen has no tab bar, but the base sizes tabNavs from
  // tabCount() and ringPos() returns a hardcoded 0 when that vector is empty.
  // Reporting one dummy tab keeps the ring model intact; buildScreen() simply
  // never calls buildTabBar(), and navigateButtons() keeps the ring off 0.
  int tabCount() const override { return isSubScreen() ? 1 : categoryCount; }
  int activeTab() const override { return isSubScreen() ? 0 : selectedCategoryIndex; }
  const char* tabLabel(int index) const override { return I18N.get(categoryNames[index]); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override;
  void navigateButtons() override;
  bool handleButtons() override;
  bool handleCustomInput() override;

  static std::string settingValueText(const SettingInfo& setting);
  void selectCategory(int categoryIndex);
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);

  void enterCategory(int categoryIndex);
  void toggleCurrentSetting();
  void openSleepTimeoutPicker();
  void rebuildSettingsLists();
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialCategory = 0);
  // Sub-screen constructor: render only the settings tagged `subCategory`, titled `subTitle`.
  SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId subCategory, StrId subTitle);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
};
