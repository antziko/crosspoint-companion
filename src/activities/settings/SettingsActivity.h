#pragma once
#include <I18n.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "SettingsSection.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

enum class SettingAction {
  None,
  RemapFrontButtons,
  RemapFrontButtonsCW,
  CustomiseStatusBar,
  CustomiseTopBar,
  ClockSettings,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  PruneCache,
  RepaginateCache,
  RebuildCovers,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  DownloadFonts,
  // Open a nested settings sub-screen listing every setting in SettingInfo::subCategory.
  // Rendered by a category-scoped SettingsActivity instance — see SettingInfo::SubScreen.
  OpenSubCategory,
  OpenControlsGroup,  // Controls > one ControlsGroup; the row's nameId is the group's title
  TextSettings,       // LOCAL(feat-dictionary): #2605 Text Settings screen
  DictNotes,          // LOCAL(feat-dictionary): the active dictionary's saved highlights
  Plugins,
  KeyboardLayouts,
  About,
  Battery,
  HomeButton,
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
    // Optional extra choice below min, off the step grid (Screen Margin's 3 before 5, 10...).
    // 0 = none. The setting's real lower bound is then `first`.
    uint8_t first = 0;
    uint8_t lowest() const { return first > 0 ? first : min; }
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)
  bool inTextSettings = false;           // Surfaced in the Text Settings screen; hidden from the flat Reader list
  bool webHidden = false;                // Persisted normally, but omitted from the generic Web Settings API

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

  SettingInfo& withWebHidden() {
    webHidden = true;
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

// Controls rows grouped by the physical control they configure. Root is the
// Controls screen itself: the group rows plus what fits no group.
enum class ControlsGroup : uint8_t { Root, Power, Front, Side, Touch };

// The rows of one SettingsSection, or of one sub-screen category; activating a
// row toggles/cycles it, opens its option popup, or launches its sub-screen.
class SettingsActivity final : public UiListActivity {
  // End on a sub-screen (subCategory_ set instead).
  const SettingsSection section = SettingsSection::End;
  // Which Controls group this screen lists; Root outside the Controls section.
  const ControlsGroup controlsGroup = ControlsGroup::Root;

  // Sub-screen mode: when subCategory_ != STR_NONE_OPT this instance lists every setting tagged
  // with that category. subTitle_ is the header/label.
  StrId subCategory_ = StrId::STR_NONE_OPT;
  StrId subTitle_ = StrId::STR_NONE_OPT;
  bool isSubScreen() const { return subCategory_ != StrId::STR_NONE_OPT; }

  // Shared list rows for this screen plus its device-only actions.
  std::vector<SettingInfo> settings;

  bool preserveQuickResumeTimeoutOn = false;
  bool quickResumeTimeoutAutoEnabled = false;

  OptionPopup optionPopup;

  // Row structure (label/actionValue), rebuilt only when the setting list
  // changes; rowValues_ holds the live value text, refreshed every
  // buildScreen() by assigning into the existing strings (no vector growth).
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;

  void rebuildSettings();
  void rebuildRowItems();
  void toggleSelectedSetting();
  void openSleepTimeoutPicker();
  void applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr);
  void syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged);
  static std::string settingValueText(const SettingInfo& setting);
  // Format one candidate value of a VALUE setting (the row's own text, and each popup row).
  static std::string settingValueOptionText(const SettingInfo& setting, uint8_t value);

  int listCount() const override { return static_cast<int>(settings.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  const char* headerTitle() const override;
  void drawFooter() override;

 public:
  SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, SettingsSection section,
                   ControlsGroup controlsGroup = ControlsGroup::Root);
  // Sub-screen constructor: render only the settings tagged `subCategory`, titled `subTitle`.
  SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId subCategory, StrId subTitle);
  void onEnter() override;
  void render(RenderLock&& lock) override;
};
