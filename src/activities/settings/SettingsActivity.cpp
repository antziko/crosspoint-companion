#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "DictionarySelectActivity.h"
#include "FontDownloadActivity.h"
#include "HomeTopBarSettingsActivity.h"
#include "KOReaderServerListActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "OpdsServerListActivity.h"
#include "OtaUpdateActivity.h"
#include "SdCardFontSystem.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatusBarSettingsActivity.h"
#include "TextSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  // Pick up any fonts uploaded/deleted over the web server since the last
  // reader activity ran — otherwise the font-family picker shows stale list.
  sdFontSystem.refreshIfDirty();
  dictionaryRegistry.refreshIfDirty();

  // Sub-screen mode: build a single flat list of every setting tagged subCategory_.
  // Reuses readerSettings as the backing vector (currentSettings points at it).
  if (isSubScreen()) {
    for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaryRegistry)) {
      if (setting.category == subCategory_) {
        // LOCAL(feat-dictionary): settings consolidated into the Text Settings screen (#2605) are
        // hidden from their sub-screen too (e.g. STR_READER_TEXT), staying only in the web list.
        if (setting.inTextSettings) continue;
        readerSettings.push_back(setting);
      }
    }
    // Device-only ACTION rows aren't in the shared (web) list, so append them per sub-group here,
    // mirroring how the top-level screen appends its actions below.
    if (subCategory_ == StrId::STR_READER_DICTIONARY) {
      // Marker-by-dwell options inline here (rather than a further nested sub-screen). These are
      // device-only — JsonSettingsIO persists them via explicit fields, not a SettingInfo key — so
      // they carry no key and are edited in place via their member pointers.
      readerSettings.push_back(
          SettingInfo::Toggle(StrId::STR_DICT_MARKER_DWELL, &CrossPointSettings::dictMarkerDwellEnabled));
      readerSettings.push_back(
          SettingInfo::Enum(StrId::STR_DICT_MARKER_T1, &CrossPointSettings::dictMarkerT1Idx,
                            {StrId::STR_SEC_3, StrId::STR_SEC_5, StrId::STR_SEC_8, StrId::STR_SEC_10}));
      readerSettings.push_back(
          SettingInfo::Enum(StrId::STR_DICT_MARKER_T2, &CrossPointSettings::dictMarkerT2Idx,
                            {StrId::STR_SEC_7, StrId::STR_SEC_9, StrId::STR_SEC_12, StrId::STR_SEC_15}));
    } else if (subCategory_ == StrId::STR_SYS_LIBRARY) {
      readerSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
      readerSettings.push_back(SettingInfo::Action(StrId::STR_REMOVE_ORPHANED_CACHES, SettingAction::PruneCache));
    } else if (subCategory_ == StrId::STR_SYS_MAINTENANCE) {
      readerSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
      readerSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
      readerSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
    }
    currentSettings = &readerSettings;
    settingsCount = static_cast<int>(currentSettings->size());
    return;
  }

  // Top-level screen: bucket the four device categories. Settings tagged with a Reader sub-group
  // category (STR_READER_TEXT / _DICTIONARY / _TRACKING) match none of these and are intentionally
  // dropped here — they surface only inside their sub-screen, reached via the SubScreen rows below.
  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaryRegistry)) {
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      // Settings merged into "Text Settings"
      // (they stay in the shared list for the web settings API)
      if (setting.inTextSettings) continue;
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      if (setting.valuePtr == &CrossPointSettings::pwrBtnFootnoteBack &&
          SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::FOOTNOTES) {
        continue;
      }
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  controlsSettings.insert(controlsSettings.begin() + 1,
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS_CW, SettingAction::RemapFrontButtonsCW));
  // System top level: keep the frequently-used Wi-Fi / Time to Sleep / KOReader Sync flat (Time to
  // Sleep arrives from the category loop as the first systemSettings entry); the rest of the System
  // items live one level down. OPDS / Clear Cache / Updates / SD Firmware / Language are appended
  // inside their sub-screen branch above.
  systemSettings.insert(systemSettings.begin(), SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_SERVERS, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::SubScreen(StrId::STR_SYS_SYNC_PROMPTS, StrId::STR_SYS_SYNC_PROMPTS));
  systemSettings.push_back(SettingInfo::SubScreen(StrId::STR_SYS_LIBRARY, StrId::STR_SYS_LIBRARY));
  systemSettings.push_back(SettingInfo::SubScreen(StrId::STR_SYS_MAINTENANCE, StrId::STR_SYS_MAINTENANCE));
  // LOCAL(feat-dictionary): #2605 — "Text Settings" opens the unified font/size/layout/style
  // screen; the font-family flat row is now hidden (inTextSettings), so this is the top Reader entry.
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_TEXT_SETTINGS, SettingAction::TextSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_MANAGE_FONTS, SettingAction::DownloadFonts));
  // Reader sub-screens: advanced text/rendering, dictionary, and reading-tracking settings live one
  // level down to keep the top-level Reader list short. Each opens a category-scoped SettingsActivity.
  readerSettings.push_back(SettingInfo::SubScreen(StrId::STR_READER_TEXT, StrId::STR_READER_TEXT));
  readerSettings.push_back(SettingInfo::SubScreen(StrId::STR_READER_DICTIONARY, StrId::STR_READER_DICTIONARY));
  readerSettings.push_back(SettingInfo::SubScreen(StrId::STR_READER_TRACKING, StrId::STR_READER_TRACKING));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  displaySettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_TOP_BAR, SettingAction::CustomiseTopBar));
  // Display sub-screens: sleep-screen/wallpaper and e-ink refresh tuning live one level down; the
  // appearance basics (theme, orientation, battery, top bar) stay flat at the Display top level.
  displaySettings.push_back(SettingInfo::SubScreen(StrId::STR_DISP_SLEEP, StrId::STR_DISP_SLEEP));
  displaySettings.push_back(SettingInfo::SubScreen(StrId::STR_DISP_EINK, StrId::STR_DISP_EINK));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Reset selection to the requested category (default 0 = Display).
  selectedCategoryIndex = initialCategory;
  selectedSettingIndex = 0;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  bool hasChangedCategory = false;

  // Row count for navigation: settings plus the tab row (top-level only; sub-screens have no tabs).
  const int rowCount = settingsCount + settingIndexBase();

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!isSubScreen() && selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (isSubScreen()) {
      // No tab row to fall back to — Back returns to the parent settings screen.
      SETTINGS.saveToFile();
      finish();
    } else if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this, rowCount] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, rowCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, rowCount] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, rowCount);
    requestUpdate();
  });

  // Side-button (continuous) category switching is a top-level affordance only.
  if (!isSubScreen()) {
    buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
      hasChangedCategory = true;
      selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
      requestUpdate();
    });

    buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
      hasChangedCategory = true;
      selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
      requestUpdate();
    });
  }

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    switch (selectedCategoryIndex) {
      case 0:
        currentSettings = &displaySettings;
        break;
      case 1:
        currentSettings = &readerSettings;
        break;
      case 2:
        currentSettings = &controlsSettings;
        break;
      case 3:
        currentSettings = &systemSettings;
        break;
    }
    settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - settingIndexBase();
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];
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
    if (setting.enumValues.size() > 2) {
      const auto valuePtr = setting.valuePtr;
      optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()),
                       currentValue, [this, valuePtr, sleepScreenChanged, quickResumeTimeoutChanged](int idx) {
                         SETTINGS.*valuePtr = idx;
                         syncQuickResumeTimeoutForSleepScreen(sleepScreenChanged, quickResumeTimeoutChanged);
                         SETTINGS.saveToFile();
                         rebuildSettingsLists();
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
      startActivityForResult(std::make_unique<DictionarySelectActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { rebuildSettingsLists(); });
      return;
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
        rebuildSettingsLists();
      };
      if (!setting.enumStringValues.empty()) {
        optionPopup.show(setting.nameId, setting.enumStringValues, cur, std::move(onSelect));
      } else {
        optionPopup.show(setting.nameId, setting.enumValues.data(), static_cast<int>(setting.enumValues.size()), cur,
                         std::move(onSelect));
      }
      requestUpdate();
      return;
    }
    setting.dyn->valueSetter((cur + 1) % totalValues);
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // uint8_t: int8_t overflows above 127, breaking dictionary history cap rollover
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::RemapFrontButtonsCW:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput, /*cwMode=*/true),
                               resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OpenSubCategory:
        // Open a nested category-scoped settings screen (reuses this activity in sub-screen mode).
        startActivityForResult(
            std::make_unique<SettingsActivity>(renderer, mappedInput, setting.subCategory, setting.nameId),
            resultHandler);
        break;
      case SettingAction::CustomiseTopBar:
        startActivityForResult(std::make_unique<HomeTopBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<OpdsServerListActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        // No-op result handler (NOT the shared one that calls SETTINGS.saveToFile()).
        // WiFi credentials live in WIFI_STORE — persisted by WifiSelectionActivity
        // itself — so there is nothing in SETTINGS to save here. The shared handler's
        // saveToFile() builds a JsonDocument, and on X3 the WiFi scan can leave heap
        // as low as ~10KB, where that allocation throws bad_alloc -> abort() (observed
        // crash on entering then cancelling the WiFi network picker).
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                               [](const ActivityResult&) {});
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::PruneCache:
        startActivityForResult(
            std::make_unique<ClearCacheActivity>(renderer, mappedInput, ClearCacheActivity::Mode::PruneOrphans),
            resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::TextSettings:
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
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
  rebuildSettingsLists();
  selectedSettingIndex = std::min(selectedSettingIndex, settingsCount);
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
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "SleepTimeoutInterval", StrId::STR_TIME_TO_SLEEP, SETTINGS.sleepTimeoutMinutes,
          CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1, 5,
          StrId::STR_SLEEP_TIMER_VALUE_FORMAT, false, true, StrId::STR_SLEEP_NEVER),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  // Sub-screens show their group name as the title; the top-level screen shows the app title+version.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isSubScreen() ? I18N.get(subTitle_) : tr(STR_SETTINGS_TITLE), isSubScreen() ? "" : CROSSPOINT_VERSION);

  // Tab bar is a top-level affordance only; sub-screens reclaim that vertical space.
  const int tabBarHeight = isSubScreen() ? 0 : metrics.tabBarHeight;
  if (!isSubScreen()) {
    std::vector<TabInfo> tabs;
    tabs.reserve(categoryCount);
    for (int i = 0; i < categoryCount; i++) {
      tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
    }
    GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                   selectedSettingIndex == 0);
  }

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - settingIndexBase(),
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          const uint8_t value = SETTINGS.*(setting.valuePtr);
          valueText = I18N.get(setting.enumValues[value]);
        } else if (setting.type == SettingType::ENUM && setting.dyn && setting.dyn->valueGetter) {
          const uint8_t value = setting.dyn->valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
            char valueBuffer[32];
            if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
              valueText = tr(STR_SLEEP_NEVER);
            } else {
              snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
                       static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
              valueText = valueBuffer;
            }
          } else if (setting.nameId == StrId::STR_LOOKUP_HIST_CAP &&
                     SETTINGS.*(setting.valuePtr) >= CrossPointSettings::HIST_CAP_UNLIMITED) {
            valueText = tr(STR_UNLIMITED);
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::ACTION && setting.dyn && setting.dyn->stringGetter) {
          valueText = setting.dyn->stringGetter();
        }
        return valueText;
      },
      true);

  // Draw help text. Top-level row 0 is the tab switcher (shows the next category name); otherwise
  // the Confirm label reflects the focused row: "Select" for pickers/actions/sub-screens, else "Toggle".
  const char* confirmLabel;
  if (!isSubScreen() && selectedSettingIndex == 0) {
    confirmLabel = I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount]);
  } else {
    const int sel = selectedSettingIndex - settingIndexBase();
    const bool selectRow = sel >= 0 && sel < settingsCount &&
                           ((*currentSettings)[sel].nameId == StrId::STR_TIME_TO_SLEEP ||
                            (*currentSettings)[sel].type == SettingType::ACTION);
    confirmLabel = selectRow ? tr(STR_SELECT) : tr(STR_TOGGLE);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
