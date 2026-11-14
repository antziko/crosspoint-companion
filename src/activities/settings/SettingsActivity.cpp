#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AboutActivity.h"
#include "BatteryActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "CrossPointSettings.h"
#include "DictionarySelectActivity.h"
#include "FontDownloadActivity.h"
#include "HomeButtonSettingsActivity.h"
#include "HomeTopBarSettingsActivity.h"
#include "KOReaderServerListActivity.h"
#include "KeyboardLayoutsActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/DictNotesActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/DictionaryRegistry.h"
#include "util/PluginEvents.h"

namespace fui = freeink::ui;

namespace {
// Section a shared-list row belongs on (End = none: sub-screen categories such
// as STR_READER_TEXT surface only inside their sub-screen).
SettingsSection sectionFor(const SettingInfo& setting) {
  if (setting.category == StrId::STR_CAT_DISPLAY) return SettingsSection::Display;
  if (setting.category == StrId::STR_CAT_READER) return SettingsSection::Reader;
  if (setting.category == StrId::STR_CAT_CONTROLS) return SettingsSection::Controls;
  if (setting.category == StrId::STR_CAT_SYSTEM) return SettingsSection::General;
  if (setting.category == StrId::STR_SYS_LIBRARY) return SettingsSection::Library;
  return SettingsSection::End;
}

// Board/state filtering on top of the section; false drops the row from the
// device UI (it stays in the shared list for the web settings API).
bool showOnDevice(const SettingInfo& setting) {
  if (home_button::isSetting(setting.valuePtr)) return false;
  // The EEGO driver ignores the fading fix's power-off (Uc8279cA4Driver::display).
  if (setting.valuePtr == &CrossPointSettings::fadingFix && BoardConfig::isEegoA4()) return false;
  // Merged into the Text Settings screen.
  if (setting.inTextSettings) return false;
  if (BoardConfig::hasHomeKey() && setting.valuePtr == &CrossPointSettings::holdConfirmAction) return false;
  if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
      SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
    return false;
  }
  return true;
}

// Group titles, indexed by ControlsGroup (Root uses the section title).
constexpr StrId CONTROLS_GROUP_TITLES[] = {StrId::STR_CAT_CONTROLS, StrId::STR_POWER_BUTTON, StrId::STR_FRONT_BUTTONS,
                                           StrId::STR_SIDE_BUTTONS, StrId::STR_TAPS_AND_GESTURES};

ControlsGroup controlsGroupOf(const SettingInfo& setting) {
  const auto field = setting.valuePtr;
  if (field == &CrossPointSettings::shortPwrBtn || field == &CrossPointSettings::powerDoubleClickAction ||
      field == &CrossPointSettings::pwrBtnFootnoteBack) {
    return ControlsGroup::Power;
  }
  // backShortToFileBrowser stays in Root: touch boards without front buttons send Back from
  // the left-edge swipe, so the row still applies there.
  if (field == &CrossPointSettings::frontButtonFollowOrientation ||
      field == &CrossPointSettings::longPressButtonBehavior || field == &CrossPointSettings::holdConfirmAction ||
      field == &CrossPointSettings::backDoubleClickAction) {
    return ControlsGroup::Front;
  }
  if (field == &CrossPointSettings::sideButtonLayout || field == &CrossPointSettings::swapSideButtonsCW ||
      field == &CrossPointSettings::sideLongPressButtonBehavior) {
    return ControlsGroup::Side;
  }
  if (field == &CrossPointSettings::touchReaderControls || field == &CrossPointSettings::pageTurnGesture ||
      field == &CrossPointSettings::previousPageGesture || field == &CrossPointSettings::showReaderMenu) {
    return ControlsGroup::Touch;
  }
  return ControlsGroup::Root;
}

ControlsGroup controlsGroupForTitle(const StrId title) {
  for (size_t i = 1; i < std::size(CONTROLS_GROUP_TITLES); ++i) {
    if (CONTROLS_GROUP_TITLES[i] == title) return static_cast<ControlsGroup>(i);
  }
  return ControlsGroup::Root;
}

// Device-only rows added per screen, beyond the shared list.
constexpr size_t EXTRA_ROWS_PER_SCREEN = 8;
}  // namespace

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const SettingsSection section, const ControlsGroup controlsGroup)
    : UiListActivity("Settings", renderer, mappedInput), section(section), controlsGroup(controlsGroup) {}

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const StrId subCategory,
                                   const StrId subTitle)
    : UiListActivity("Settings", renderer, mappedInput), subCategory_(subCategory), subTitle_(subTitle) {}

void SettingsActivity::rebuildSettings() {
  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();
  dictionaryRegistry.refreshIfDirty();

  auto all = getSettingsList(&sdFontSystem.registry(), &dictionaryRegistry);
  const auto belongs = [this](const SettingInfo& setting) {
    if (isSubScreen()) return setting.category == subCategory_ && !setting.inTextSettings;
    return sectionFor(setting) == section && showOnDevice(setting) &&
           (section != SettingsSection::Controls || controlsGroupOf(setting) == controlsGroup);
  };
  settings.clear();
  settings.reserve(std::count_if(all.begin(), all.end(), belongs) + EXTRA_ROWS_PER_SCREEN);
  const auto addAction = [this](StrId name, SettingAction action) {
    settings.push_back(SettingInfo::Action(name, action));
  };
  const auto addShared = [this, &all, &belongs] {
    for (auto& setting : all) {
      if (belongs(setting)) settings.push_back(std::move(setting));
    }
  };

  if (isSubScreen()) {
    addShared();
    // Device-only ACTION rows aren't in the shared (web) list, so append them per sub-group here.
    if (subCategory_ == StrId::STR_READER_DICTIONARY) {
      // Marker-by-dwell options inline here (rather than a further nested sub-screen). These are
      // device-only — CrossPointSettings::toJson/fromJson persists them via explicit fields, not a SettingInfo key — so
      // they carry no key and are edited in place via their member pointers.
      settings.push_back(
          SettingInfo::Toggle(StrId::STR_DICT_MARKER_DWELL, &CrossPointSettings::dictMarkerDwellEnabled));
      settings.push_back(
          SettingInfo::Toggle(StrId::STR_DICT_INLINE_GLOSS, &CrossPointSettings::dictInlineGlossEnabled));
      settings.push_back(SettingInfo::Enum(StrId::STR_DICT_MARKER_T1, &CrossPointSettings::dictMarkerT1Idx,
                                           {StrId::STR_SEC_3, StrId::STR_SEC_5, StrId::STR_SEC_8, StrId::STR_SEC_10}));
      settings.push_back(SettingInfo::Enum(StrId::STR_DICT_MARKER_T2, &CrossPointSettings::dictMarkerT2Idx,
                                           {StrId::STR_SEC_7, StrId::STR_SEC_9, StrId::STR_SEC_12, StrId::STR_SEC_15}));
      // Notes are scoped to the dictionary, so they are reachable with no book open. The reader
      // menu has the same row for the book's active dictionary.
      addAction(StrId::STR_DICT_NOTES, SettingAction::DictNotes);
    }
    rebuildRowItems();
    return;
  }

  switch (section) {
    case SettingsSection::General:
      // Language leads so a user stuck in an unfamiliar language finds it first.
      addAction(StrId::STR_LANGUAGE, SettingAction::Language);
      addAction(StrId::STR_KEYBOARD_LAYOUTS, SettingAction::KeyboardLayouts);
      addShared();
      // Clock configuration (timezone, DST, format, manual sync). Shown wherever the device CAN
      // have a clock: a board with an RTC, or one that gets time from NTP over WiFi. Upstream
      // gates on halClock.isAvailable(), which is false on an RTC-less board until the first
      // sync — and this screen is where that sync is started, so that gate would hide itself.
      if (halClock.isAvailable() || !halClock.hasHardwareRtc()) {
        addAction(StrId::STR_CLOCK, SettingAction::ClockSettings);
      }
      addAction(StrId::STR_BATTERY, SettingAction::Battery);
      // OTA fetches this board's own release asset (see OtaUpdater); boards whose
      // asset isn't published yet just report no update available.
      addAction(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates);
      addAction(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate);
      addAction(StrId::STR_ABOUT, SettingAction::About);
      break;
    case SettingsSection::Display:
      addShared();
      addAction(StrId::STR_CUSTOMISE_TOP_BAR, SettingAction::CustomiseTopBar);
      // Sleep-screen/wallpaper and e-ink refresh tuning live one level down; the appearance
      // basics (theme, orientation, battery, top bar) stay flat here.
      settings.push_back(SettingInfo::SubScreen(StrId::STR_DISP_SLEEP, StrId::STR_DISP_SLEEP));
      settings.push_back(SettingInfo::SubScreen(StrId::STR_DISP_EINK, StrId::STR_DISP_EINK));
      break;
    case SettingsSection::Reader:
      // LOCAL(feat-dictionary): #2605 — "Text Settings" opens the unified font/size/layout/style
      // screen; the font-family flat row is hidden (inTextSettings), so this is the top entry.
      addAction(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings);
      addAction(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts);
      addShared();
      // Advanced text/rendering, dictionary, and reading-tracking settings live one level down
      // to keep this list short. Each opens a category-scoped SettingsActivity.
      settings.push_back(SettingInfo::SubScreen(StrId::STR_READER_TEXT, StrId::STR_READER_TEXT));
      settings.push_back(SettingInfo::SubScreen(StrId::STR_READER_DICTIONARY, StrId::STR_READER_DICTIONARY));
      settings.push_back(SettingInfo::SubScreen(StrId::STR_READER_TRACKING, StrId::STR_READER_TRACKING));
      addAction(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar);
      break;
    case SettingsSection::Controls:
      if (controlsGroup == ControlsGroup::Root) {
        if (BoardConfig::hasHomeKey()) addAction(StrId::STR_HOME_BUTTON, SettingAction::HomeButton);
        // A group row only where it has rows. Front buttons also needs the buttons themselves:
        // on a board without them (X4 Pro) its rows configure nothing.
        for (size_t i = 1; i < std::size(CONTROLS_GROUP_TITLES); ++i) {
          const auto group = static_cast<ControlsGroup>(i);
          if (group == ControlsGroup::Front && !mappedInput.hasFrontButtons()) continue;
          const bool hasRows = std::any_of(all.begin(), all.end(), [group](const SettingInfo& setting) {
            return sectionFor(setting) == SettingsSection::Controls && showOnDevice(setting) &&
                   controlsGroupOf(setting) == group;
          });
          if (hasRows) addAction(CONTROLS_GROUP_TITLES[i], SettingAction::OpenControlsGroup);
        }
      } else if (controlsGroup == ControlsGroup::Front) {
        // ButtonRemapActivity captures raw front presses to assign the four roles.
        addAction(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons);
        addAction(StrId::STR_REMAP_FRONT_BUTTONS_CW, SettingAction::RemapFrontButtonsCW);
      }
      addShared();
      break;
    case SettingsSection::Library:
      addShared();
      addAction(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache);
      addAction(StrId::STR_REMOVE_ORPHANED_CACHES, SettingAction::PruneCache);
      addAction(StrId::STR_REPAGINATE_BOOKS, SettingAction::RepaginateCache);
      addAction(StrId::STR_REBUILD_COVERS, SettingAction::RebuildCovers);
      break;
    case SettingsSection::Network:
      addAction(StrId::STR_WIFI_NETWORKS, SettingAction::Network);
      addAction(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync);
      addAction(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser);
      addAction(StrId::STR_PLUGINS, SettingAction::Plugins);
      settings.push_back(SettingInfo::SubScreen(StrId::STR_SYS_SYNC_PROMPTS, StrId::STR_SYS_SYNC_PROMPTS));
      break;
    case SettingsSection::End:
      break;
  }
  rebuildRowItems();
}

void SettingsActivity::onEnter() {
  UiListActivity::onEnter();

  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettings();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for settings.
// Structural — call only when the setting list changes, never from
// buildScreen(), which only refreshes rowValues_ content and rowItems_[].value
// pointers in place.
void SettingsActivity::rebuildRowItems() {
  rowValues_.assign(settings.size(), std::string());
  rowItems_.clear();
  rowItems_.reserve(settings.size());
  for (size_t i = 0; i < settings.size(); i++) {
    fui::ListItem item;
    item.label = I18N.get(settings[i].nameId);
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
}

const char* SettingsActivity::headerTitle() const {
  if (isSubScreen()) return I18N.get(subTitle_);
  if (controlsGroup != ControlsGroup::Root) return I18N.get(CONTROLS_GROUP_TITLES[static_cast<size_t>(controlsGroup)]);
  return I18N.get(SETTINGS_SECTION_TITLES[static_cast<size_t>(section)]);
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  nav.selected = index;
  toggleSelectedSetting();
  // Tap-first: a tapped row is not a cursor position. Left selected, it would stay
  // highlighted once its popup or sub-screen closed; row 0 is hidden by ListCursor until a
  // nav button is pressed, and the viewport does not follow a plain selection change.
  if (mappedInput.hasTouch()) nav.selected = 0;
  requestUpdate();
}

void SettingsActivity::onBackButton() {
  SETTINGS.saveToFile();
  finish();
}

void SettingsActivity::applyUiSettingChange(uint8_t CrossPointSettings::* valuePtr) {
  // Theme changes take effect immediately, on this screen — reload the theme
  // and re-derive the app's tokens so the very next repaint is in the new look.
  if (valuePtr != &CrossPointSettings::uiTheme) {
    return;
  }
  UITheme::getInstance().reload();
  // Re-derive the shared tokens for the new look; the gate stays closed until
  // the repaint that rebuilds the interaction table in the new layout.
  resetUi();
}

bool SettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void SettingsActivity::toggleSelectedSetting() {
  mappedInput.resetHomeButtonInput();
  const int selectedSetting = nav.selected;
  if (selectedSetting < 0 || selectedSetting >= static_cast<int>(settings.size())) {
    return;
  }

  const auto& setting = settings[selectedSetting];
  const bool sleepScreenChanged = setting.valuePtr == &CrossPointSettings::sleepScreen;
  const bool quickResumeTimeoutChanged = setting.valuePtr == &CrossPointSettings::quickResumeSleepScreen;

  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    openSleepTimeoutPicker();
    return;
  }

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const auto& enumLabels = setting.enumValues;
    if (enumLabels.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), currentValue,
                       [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         SETTINGS.saveToFile();
                         rebuildSettings();
                         applyUiSettingChange(valuePtr);
                       });
      requestUpdate();
      return;
    }
    SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
  } else if (setting.type == SettingType::ENUM && setting.dyn && setting.dyn->valueGetter && setting.dyn->valueSetter) {
    // LOCAL(feat-dictionary): font-family no longer launches FontSelectionActivity from here — it is
    // hidden from the flat Reader list (inTextSettings) and now lives in the Text Settings Font tab (#2605).
    if (setting.nameId == StrId::STR_DICTIONARY) {
      // Launch the dictionary picker (rich metadata/preparation flow) instead of cycling.
      // The picker writes the selection to dictionary.bin itself; just refresh on return.
      startActivityForResultNoThrow<DictionarySelectActivity>([this](const ActivityResult&) { rebuildSettings(); },
                                                              renderer, mappedInput);
      return;
    }
    if (setting.nameId == StrId::STR_DICT_FONT_SIZE) {
      const std::vector<uint8_t> sizes = dictFontPointSizes();
      if (isContiguousPointRange(sizes)) {
        startActivityForResultNoThrow<IntervalSelectionActivity>(
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                SETTINGS.dictionaryFontPointSize = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
                SETTINGS.saveToFile();
                rebuildSettings();
              }
              requestUpdate();
            },
            renderer, mappedInput, "DictFontSize", StrId::STR_DICT_FONT_SIZE,
            snapToNearestPointSize(sizes, SETTINGS.dictionaryFontPointSize), sizes.front(), sizes.back(), 1, 2,
            StrId::STR_POINT_SIZE_VALUE_FORMAT, false, true);
        return;
      }
    }
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    // Mirror the valuePtr enum path above: settings with >2 options open the Selection
    // Popup (#2358); 1-2 option settings still cycle in place. Adapted to the out-of-line
    // dyn accessors (guaranteed non-null by the else-if guard above).
    const uint8_t cur = setting.dyn->valueGetter();
    if (totalValues > 2) {
      const auto valueSetter = setting.dyn->valueSetter;
      auto onSelect = [this, valueSetter, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
        valueSetter(idx);
        syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
        SETTINGS.saveToFile();
        rebuildSettings();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        const auto& enumLabels = setting.enumValues;
        optionPopup.show(setting.nameId, enumLabels.data(), static_cast<int>(enumLabels.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.dyn->valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // uint8_t: int8_t overflows above 127, breaking dictionary history cap rollover
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    // Same treatment as multi-option enums (#2358): show the choices instead of making the
    // user press Confirm through them one at a time. The labels are generated here rather
    // than being StrIds, so the stored value stays the raw number -- no index remapping, and
    // nothing to migrate. Only when the whole range fits the popup: OptionPopup does not
    // scroll, so a longer range (UTC offset at 105 steps, brightness/warmth at 21) would put
    // values out of reach entirely. Those keep cycling until the popup can scroll.
    const int step = setting.valueRange.step > 0 ? setting.valueRange.step : 1;
    const int span = static_cast<int>(setting.valueRange.max) - static_cast<int>(setting.valueRange.min);
    const int optionCount = span >= 0 ? (span / step) + 1 : 0;
    if (optionCount > 2 && optionCount <= OptionPopup::MAX_OPTIONS) {
      std::vector<std::string> labels;
      labels.reserve(optionCount);
      int currentIndex = 0;
      for (int i = 0; i < optionCount; i++) {
        const uint8_t v = static_cast<uint8_t>(setting.valueRange.min + i * step);
        if (v <= currentValue) currentIndex = i;  // nearest option at or below a stale value
        labels.push_back(settingValueOptionText(setting, v));
      }
      const auto valuePtr = setting.valuePtr;
      const uint8_t minValue = setting.valueRange.min;
      optionPopup.show(setting.nameId, labels, currentIndex, [this, valuePtr, minValue, step](int idx) {
        SETTINGS.*valuePtr = static_cast<uint8_t>(minValue + idx * step);
        SETTINGS.saveToFile();
        rebuildSettings();
        applyUiSettingChange(valuePtr);
      });
      requestUpdate();
      return;
    }
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::HomeButton: {
        // Activities must outlive this call and are owned by the activity stack.
        auto activity = makeUniqueNoThrow<HomeButtonSettingsActivity>(renderer, mappedInput);
        if (!activity) {
          LOG_ERR("SET", "OOM: Home button settings");
          return;
        }
        startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
        return;
      }
      case SettingAction::RemapFrontButtons:
        startActivityForResultNoThrow<ButtonRemapActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::RemapFrontButtonsCW:
        startActivityForResultNoThrow<ButtonRemapActivity>(resultHandler, renderer, mappedInput, /*cwMode=*/true);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResultNoThrow<StatusBarSettingsActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::OpenSubCategory:
        // Open a nested category-scoped settings screen (reuses this activity in sub-screen mode).
        startActivityForResultNoThrow<SettingsActivity>(resultHandler, renderer, mappedInput, setting.subCategory,
                                                        setting.nameId);
        break;
      case SettingAction::OpenControlsGroup:
        startActivityForResultNoThrow<SettingsActivity>(resultHandler, renderer, mappedInput, SettingsSection::Controls,
                                                        controlsGroupForTitle(setting.nameId));
        break;
      case SettingAction::CustomiseTopBar:
        startActivityForResultNoThrow<HomeTopBarSettingsActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::ClockSettings:
        if (auto activity = makeUniqueNoThrow<ClockSettingsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), resultHandler);
        } else {
          LOG_ERR("SETTINGS", "OOM: ClockSettingsActivity");
        }
        break;
      case SettingAction::KOReaderSync:
        startActivityForResultNoThrow<KOReaderServerListActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResultNoThrow<OpdsServerListActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::Network:
        // NOT the shared result handler: it calls SETTINGS.saveToFile(). WiFi credentials
        // live in WIFI_STORE — persisted by WifiSelectionActivity itself — so there is
        // nothing in SETTINGS to save here. The shared handler's saveToFile() builds a
        // JsonDocument, and on X3 the WiFi scan can leave heap as low as ~10KB, where that
        // allocation throws bad_alloc -> abort() (observed crash on entering then
        // cancelling the WiFi network picker).
        startActivityForResultNoThrow<WifiSelectionActivity>(
            [](const ActivityResult&) {
              // Every other WiFi consumer hands the radio to a session that owns it and
              // tears it down in its own onExit(). This picker is a CHILD of Settings, so
              // that parent exit never happens and nothing releases the driver's heap —
              // scanning alone brings the radio up, so tear down whether or not the user
              // joined anything.
              if (WiFi.getMode() == WIFI_MODE_NULL) return;
              WiFi.disconnect(false);
              delay(30);
              // Unlike the onExit() teardowns this runs from the loop task with no lock
              // held, so the restart popup needs one to paint.
              RenderLock lock;
              silentRestartToSettings(SettingsSection::Network);
            },
            renderer, mappedInput);
        break;
      case SettingAction::ClearCache:
        startActivityForResultNoThrow<ClearCacheActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::PruneCache:
        startActivityForResultNoThrow<ClearCacheActivity>(resultHandler, renderer, mappedInput,
                                                          ClearCacheActivity::Mode::PruneOrphans);
        break;
      case SettingAction::RepaginateCache:
        startActivityForResultNoThrow<ClearCacheActivity>(resultHandler, renderer, mappedInput,
                                                          ClearCacheActivity::Mode::RepaginateAll);
        break;
      case SettingAction::RebuildCovers:
        startActivityForResultNoThrow<ClearCacheActivity>(resultHandler, renderer, mappedInput,
                                                          ClearCacheActivity::Mode::RebuildCovers);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResultNoThrow<OtaUpdateActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResultNoThrow<SdFirmwareUpdateActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResultNoThrow<FontDownloadActivity>(
            [this](const ActivityResult&) {
              SETTINGS.saveToFile();
              rebuildSettings();
            },
            renderer, mappedInput);
        break;
      case SettingAction::TextSettings:
        startActivityForResultNoThrow<TextSettingsActivity>(
            [this](const ActivityResult&) {
              SETTINGS.saveToFile();
              rebuildSettings();
            },
            renderer, mappedInput, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family);
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildSettings() and nothing
        // re-runs it on Pop (there is no onResume), so a language switch needs an
        // explicit rebuild here rather than the generic resultHandler.
        startActivityForResultNoThrow<LanguageSelectActivity>(
            [this](const ActivityResult&) {
              SETTINGS.saveToFile();
              rebuildSettings();
            },
            renderer, mappedInput);
        break;
      case SettingAction::Plugins: {
        static constexpr StrId OPTIONS[] = {StrId::STR_ENABLED, StrId::STR_DISABLED, StrId::STR_CANCEL};
        optionPopup.show(StrId::STR_PLUGINS, OPTIONS, 3, SETTINGS.pluginsEnabled ? 0 : 1, [this](const int idx) {
          if (idx == 2) return;
          SETTINGS.pluginsEnabled = idx == 0 ? 1 : 0;
          // Turning plugins on brings back a dismissed Plugin Hub install row.
          if (SETTINGS.pluginsEnabled) SETTINGS.pluginHubPromptHidden = 0;
          SETTINGS.saveToFile();
          // Applies to event delivery now, not after a reboot.
          pluginevents::refreshSubscriptions();
          rebuildSettings();
        });
        requestUpdate();
        return;
      }
      case SettingAction::KeyboardLayouts:
        if (auto activity = makeUniqueNoThrow<KeyboardLayoutsActivity>(renderer, mappedInput)) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: KeyboardLayoutsActivity");
        }
        break;
      case SettingAction::DictNotes: {
        // No book here, so the GLOBAL selection (activeDictPath with no cache path) decides
        // whose notes these are.
        const std::string dictPath = Dictionary::activeDictPath(nullptr);
        const uint32_t dictHash = DictUtils::dictHashOfPath(dictPath);
        if (dictHash == 0) {
          GUI.drawPopup(renderer, tr(STR_DICT_NONE));  // refreshes internally
          activityManager.holdForReading(900);
          requestUpdate();
          break;
        }
        if (auto activity = makeUniqueNoThrow<DictNotesActivity>(renderer, mappedInput, dictHash,
                                                                 DictUtils::dictDisplayName(dictPath))) {
          startActivityForResult(std::move(activity), nullptr);
        } else {
          LOG_ERR("SETTINGS", "OOM: DictNotesActivity");
        }
        break;
      }
      case SettingAction::About:
        startActivityForResultNoThrow<AboutActivity>([](const ActivityResult&) {}, renderer, mappedInput);
        break;
      case SettingAction::Battery:
        startActivityForResultNoThrow<BatteryActivity>([](const ActivityResult&) {}, renderer, mappedInput);
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
  SETTINGS.saveToFile();
  const auto valuePtr = setting.valuePtr;  // rebuildSettings() replaces `setting`
  rebuildSettings();
  applyUiSettingChange(valuePtr);
  // Rows can appear/disappear with a change (e.g. the footnote-back toggle).
  nav.selected = std::min(selectedSetting, static_cast<int>(settings.size()) - 1);
}

void SettingsActivity::syncQuickResumeTimeoutForSleepScreen(bool sleepScreenChanged, bool quickResumeTimeoutChanged) {
  if (quickResumeTimeoutChanged) {
    preserveQuickResumeTimeoutOn =
        SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
    quickResumeTimeoutAutoEnabled = false;
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME) {
    if (SETTINGS.quickResumeSleepScreen != CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT) {
      SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
      quickResumeTimeoutAutoEnabled = !preserveQuickResumeTimeoutOn;
    } else if (sleepScreenChanged && !preserveQuickResumeTimeoutOn) {
      quickResumeTimeoutAutoEnabled = true;
    }
    return;
  }

  if (sleepScreenChanged && quickResumeTimeoutAutoEnabled && !preserveQuickResumeTimeoutOn) {
    SETTINGS.quickResumeSleepScreen = CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER;
    quickResumeTimeoutAutoEnabled = false;
  }
}

void SettingsActivity::openSleepTimeoutPicker() {
  startActivityForResultNoThrow<IntervalSelectionActivity>(
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      },
      renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
      CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
      StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true, StrId::STR_SLEEP_NEVER);
}

// Render ONE candidate value of a VALUE setting. Split out of settingValueText so the same
// per-setting formatting (sentinels, units) labels both the row and every row of the
// selection popup — a popup that showed raw numbers where the row says "Unlimited" / "Off"
// would be a different setting as far as the user is concerned.
std::string SettingsActivity::settingValueOptionText(const SettingInfo& setting, uint8_t value) {
  if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
    if (value >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
      return tr(STR_SLEEP_NEVER);
    }
    char valueBuffer[32];
    snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT), static_cast<unsigned int>(value));
    return valueBuffer;
  }
  // LOCAL(feat): the lookup-history cap renders as "Unlimited" past its
  // sentinel rather than as a raw number. Upstream has no such row, so its
  // settingValueText drops this.
  if (setting.nameId == StrId::STR_LOOKUP_HIST_CAP && value >= CrossPointSettings::HIST_CAP_UNLIMITED) {
    return tr(STR_UNLIMITED);
  }
  // The re-count window is a duration, and 0 means "no window" rather than "0 minutes".
  // Shares the sleep timer's "%u min" format instead of adding a second one.
  if (setting.nameId == StrId::STR_FC_RECOUNT_WINDOW) {
    if (value == 0) return tr(STR_STATE_OFF);
    char valueBuffer[32];
    snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT), static_cast<unsigned int>(value));
    return valueBuffer;
  }
  return std::to_string(value);
}

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (setting.action == SettingAction::HomeButton) return tr(STR_CONFIGURE);
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    return SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }
  if (setting.action == SettingAction::Plugins) return SETTINGS.pluginsEnabled ? tr(STR_ENABLED) : tr(STR_DISABLED);
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    const uint8_t value = SETTINGS.*(setting.valuePtr);
    const auto& enumLabels = setting.enumValues;
    if (value >= enumLabels.size()) return "";
    return I18N.get(enumLabels[value]);
  }
  // LOCAL(feat): the runtime getters live in the optional `dyn` sub-struct
  // here, not inline on SettingInfo as upstream has them.
  if (setting.type == SettingType::ENUM && setting.dyn && setting.dyn->valueGetter) {
    const uint8_t value = setting.dyn->valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    const auto& enumLabels = setting.enumValues;
    if (value < enumLabels.size()) {
      return I18N.get(enumLabels[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    return settingValueOptionText(setting, SETTINGS.*(setting.valuePtr));
  }
  // LOCAL(feat): ACTION rows can carry a live value string (e.g. the current
  // language, the active dictionary). Upstream's settingValueText has no
  // ACTION branch at all, so every one of these rows would render blank.
  if (setting.type == SettingType::ACTION && setting.dyn && setting.dyn->stringGetter) {
    return setting.dyn->stringGetter();
  }
  return "";
}

void SettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the header band, clear of the button hints in any orientation.
  screen.setContentMargin(safeContentInsets(renderer, metrics.topPadding + metrics.headerHeight));

  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_ (label/actionValue) was built by rebuildRowItems(); only the
  // live value text needs refreshing here, by assigning into the existing
  // rowValues_ strings (no vector growth).
  // Self-heal rather than index past the end: the loop below writes by row with
  // no bounds check, and a missed rebuild would corrupt the heap instead of just
  // drawing the wrong list.
  if (rowItems_.size() != settings.size() || rowValues_.size() != settings.size()) {
    rebuildRowItems();
  }
  for (size_t i = 0; i < settings.size(); i++) {
    const auto& setting = settings[i];
    const auto& labels = setting.enumValues;
    const bool hasGetter = setting.dyn && setting.dyn->valueGetter;
    const bool checkbox = setting.type == SettingType::TOGGLE ||
                          (setting.type == SettingType::ENUM && setting.enumStringValues.empty() &&
                           labels.size() == 2 && labels[0] == StrId::STR_STATE_OFF && labels[1] == StrId::STR_STATE_ON);
    if (checkbox && (setting.valuePtr || hasGetter)) {
      const bool checked = setting.valuePtr ? SETTINGS.*(setting.valuePtr) != 0 : setting.dyn->valueGetter() != 0;
      rowValues_[i].clear();
      GUI.setCheckboxRow(rowItems_[i], checked);
    } else {
      rowValues_[i] = settingValueText(setting);
      rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
      rowItems_[i].toggle = false;  // a row can switch between checkbox and value text
    }
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // No valueInset: sidePadding already insets both edges of the row, so any
  // extra here lands on the trailing side only and the value sits further
  // from the edge than the label does.
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  // maxLines=2 also marks the style explicitly set (an all-default smallText
  // fails textStyleUnset and the list would substitute bodyText back); the
  // common fits-on-one-line case takes the renderer's fast path anyway.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void SettingsActivity::drawFooter() {
  // The Confirm label reflects the focused row: "Select" for pickers and
  // actions, else "Toggle".
  // LOCAL(feat): ACTION rows say "Select" too — upstream only special-cased
  // the sleep-timeout picker, so every sub-screen and action row read
  // "Toggle" even though Confirm opens something.
  const int sel = nav.selected;
  const bool selectRow =
      sel >= 0 && sel < static_cast<int>(settings.size()) &&
      (settings[sel].nameId == StrId::STR_TIME_TO_SLEEP || settings[sel].type == SettingType::ACTION);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), selectRow ? tr(STR_SELECT) : tr(STR_TOGGLE), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  // Through the base so the build runs inside renderListFrame's
  // consumeRebuildNeeded() loop; drawing renderUi() once left a viewport that
  // onListRendered had corrected unpainted, which is the settings list that
  // would not scroll.
  UiListActivity::render(std::move(lock));
}
