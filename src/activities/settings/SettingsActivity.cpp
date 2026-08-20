#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
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
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

namespace fui = freeink::ui;

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const int initialCategory)
    : UiTabListActivity("Settings", renderer, mappedInput), initialCategory(initialCategory) {}

SettingsActivity::SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const StrId subCategory,
                                   const StrId subTitle)
    : UiTabListActivity("Settings", renderer, mappedInput), subCategory_(subCategory), subTitle_(subTitle) {}

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
      // device-only — CrossPointSettings::toJson/fromJson persists them via explicit fields, not a SettingInfo key — so
      // they carry no key and are edited in place via their member pointers.
      readerSettings.push_back(
          SettingInfo::Toggle(StrId::STR_DICT_MARKER_DWELL, &CrossPointSettings::dictMarkerDwellEnabled));
      readerSettings.push_back(
          SettingInfo::Toggle(StrId::STR_DICT_INLINE_GLOSS, &CrossPointSettings::dictInlineGlossEnabled));
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
    // A sub-screen never calls selectCategory(), so this is the only place its
    // rows get built. buildScreen() indexes rowValues_/rowItems_ by row without
    // a bounds check, so skipping this would write past two empty vectors.
    rebuildRowItems();
    return;
  }

  // Top-level screen: bucket the four device categories. Settings tagged with a Reader sub-group
  // category (STR_READER_TEXT / _DICTIONARY / _TRACKING) match none of these and are intentionally
  // dropped here — they surface only inside their sub-screen, reached via the SubScreen rows below.
  for (auto& setting : getSettingsList(&sdFontSystem.registry(), &dictionaryRegistry)) {
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      // The sunlight fading fix is a grayscale-waveform compensation that does
      // not apply on the X4 Pro (plain OTP waveform, no custom grayscale LUT).
      if (setting.valuePtr == &CrossPointSettings::fadingFix && BoardConfig::isX4Pro()) {
        continue;
      }
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
  rebuildRowItems();
}

void SettingsActivity::onEnter() {
  UiTabListActivity::onEnter();

  // LOCAL(feat): open on the requested category (default 0 = Display), not
  // always the first. Ring position comes from the base's per-tab nav reset:
  // 0 (the tab bar) in tab mode, but a sub-screen has no tab bar, so its ring
  // starts on the first row.
  selectedCategoryIndex = isSubScreen() ? 0 : initialCategory;
  if (isSubScreen()) activeNav().selected = 1;
  preserveQuickResumeTimeoutOn =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  quickResumeTimeoutAutoEnabled = false;
  syncQuickResumeTimeoutForSleepScreen(/*sleepScreenChanged=*/true, /*quickResumeTimeoutChanged=*/false);

  rebuildSettingsLists();
}

void SettingsActivity::selectCategory(const int categoryIndex) {
  selectedCategoryIndex = categoryIndex;
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
  activeNav().top = 0;  // category switches start the list at the top (no per-tab memory here)
  rebuildRowItems();
}

// Rebuilds rowValues_/rowItems_ (label + actionValue) for *currentSettings.
// Structural — call only when the active category or a category's setting
// list changes, never from buildScreen(), which only refreshes rowValues_
// content and rowItems_[].value pointers in place.
void SettingsActivity::rebuildRowItems() {
  const auto& settings = *currentSettings;
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

void SettingsActivity::onTabAction(const int index) {
  if (optionPopup.isActive()) return;
  selectCategory(index);
  activeNav().selected = 0;  // tab taps land with the tab bar focused
  // The switched-to tab repaints as the selected pill; a flash overlay on top
  // of it just repaints the pill in the focused style.
  app.clearTapFlash();
}

void SettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  (void)index;  // toggleCurrentSetting reads the ring position
  // Most rows repaint a different surface (popup, sub-activity, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  toggleCurrentSetting();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
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

void SettingsActivity::stepTab(const int direction) {
  // LOCAL(feat): a sub-screen shows one fixed category, so there is nothing to
  // step to. Side-button category switching was always a top-level affordance.
  if (isSubScreen()) return;
  // Ring position 0 stays on the tab bar; a row selection collapses to the
  // new category's first row (per-tab memory is deliberately not kept here).
  const bool onTabBar = ringPos() == 0;
  selectedCategoryIndex = direction > 0 ? ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount)
                                        : ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
  selectCategory(selectedCategoryIndex);
  activeNav().selected = onTabBar ? 0 : 1;
  requestUpdate();
}

// LOCAL(feat): a sub-screen has no tab bar, so its ring must stay on the rows
// (1..listCount) instead of wrapping through position 0. Tab mode keeps the
// base's ring walk verbatim.
void SettingsActivity::navigateButtons() {
  if (!isSubScreen()) {
    UiTabListActivity::navigateButtons();
    return;
  }
  const int count = listCount();
  if (count <= 0) return;
  buttonNavigator.onNextRelease([this, count] { moveRingTo(ButtonNavigator::nextIndex(ringPos() - 1, count) + 1); });
  buttonNavigator.onPreviousRelease(
      [this, count] { moveRingTo(ButtonNavigator::previousIndex(ringPos() - 1, count) + 1); });
}

bool SettingsActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!isSubScreen() && ringPos() == 0) {
      stepTab(1);
    } else {
      toggleCurrentSetting();
      requestUpdate();
    }
    return true;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (isSubScreen()) {
      // No tab row to fall back to — Back returns to the parent settings screen.
      SETTINGS.saveToFile();
      finish();
    } else if (ringPos() > 0) {
      activeNav().selected = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return true;
  }

  return false;
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = ringPos() - 1;
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
      startActivityForResultNoThrow<DictionarySelectActivity>([this](const ActivityResult&) { rebuildSettingsLists(); },
                                                              renderer, mappedInput);
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
      case SettingAction::CustomiseTopBar:
        startActivityForResultNoThrow<HomeTopBarSettingsActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResultNoThrow<KOReaderServerListActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResultNoThrow<OpdsServerListActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::Network:
        // No-op result handler (NOT the shared one that calls SETTINGS.saveToFile()).
        // WiFi credentials live in WIFI_STORE — persisted by WifiSelectionActivity
        // itself — so there is nothing in SETTINGS to save here. The shared handler's
        // saveToFile() builds a JsonDocument, and on X3 the WiFi scan can leave heap
        // as low as ~10KB, where that allocation throws bad_alloc -> abort() (observed
        // crash on entering then cancelling the WiFi network picker).
        startActivityForResultNoThrow<WifiSelectionActivity>([](const ActivityResult&) {}, renderer, mappedInput);
        break;
      case SettingAction::ClearCache:
        startActivityForResultNoThrow<ClearCacheActivity>(resultHandler, renderer, mappedInput);
        break;
      case SettingAction::PruneCache:
        startActivityForResultNoThrow<ClearCacheActivity>(resultHandler, renderer, mappedInput,
                                                          ClearCacheActivity::Mode::PruneOrphans);
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
              rebuildSettingsLists();
            },
            renderer, mappedInput);
        break;
      case SettingAction::TextSettings:
        startActivityForResultNoThrow<TextSettingsActivity>(
            [this](const ActivityResult&) {
              SETTINGS.saveToFile();
              rebuildSettingsLists();
            },
            renderer, mappedInput, &sdFontSystem.registry(), TextSettingsActivity::Tab::Family);
        break;
      case SettingAction::Language:
        // Row labels are translated once in rebuildSettingsLists() and nothing
        // re-runs it on Pop (there is no onResume), so a language switch needs an
        // explicit rebuild here rather than the generic resultHandler.
        startActivityForResultNoThrow<LanguageSelectActivity>(
            [this](const ActivityResult&) {
              SETTINGS.saveToFile();
              rebuildSettingsLists();
            },
            renderer, mappedInput);
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
  applyUiSettingChange(setting.valuePtr);
  activeNav().selected = std::min(ringPos(), settingsCount);
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

std::string SettingsActivity::settingValueText(const SettingInfo& setting) {
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    return SETTINGS.*(setting.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  }
  if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Guard like the valueGetter branch below: a corrupt/migrated settings
    // byte must not index past the enum table.
    const uint8_t value = SETTINGS.*(setting.valuePtr);
    if (value >= setting.enumValues.size()) return "";
    return I18N.get(setting.enumValues[value]);
  }
  // LOCAL(feat): the runtime getters live in the optional `dyn` sub-struct
  // here, not inline on SettingInfo as upstream has them.
  if (setting.type == SettingType::ENUM && setting.dyn && setting.dyn->valueGetter) {
    const uint8_t value = setting.dyn->valueGetter();
    if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
      return setting.enumStringValues[value];
    }
    if (value < setting.enumValues.size()) {
      return I18N.get(setting.enumValues[value]);
    }
    return "";
  }
  if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (setting.nameId == StrId::STR_TIME_TO_SLEEP) {
      if (SETTINGS.sleepTimeoutMinutes >= CrossPointSettings::SLEEP_TIMEOUT_NEVER_MINUTES) {
        return tr(STR_SLEEP_NEVER);
      }
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    // LOCAL(feat): the lookup-history cap renders as "Unlimited" past its
    // sentinel rather than as a raw number. Upstream has no such row, so its
    // settingValueText drops this.
    if (setting.nameId == StrId::STR_LOOKUP_HIST_CAP &&
        SETTINGS.*(setting.valuePtr) >= CrossPointSettings::HIST_CAP_UNLIMITED) {
      return tr(STR_UNLIMITED);
    }
    // The re-count window is a duration, and 0 means "no window" rather than "0 minutes".
    // Shares the sleep timer's "%u min" format instead of adding a second one.
    if (setting.nameId == StrId::STR_FC_RECOUNT_WINDOW) {
      if (SETTINGS.*(setting.valuePtr) == 0) return tr(STR_STATE_OFF);
      char valueBuffer[32];
      snprintf(valueBuffer, sizeof(valueBuffer), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
               static_cast<unsigned int>(SETTINGS.*(setting.valuePtr)));
      return valueBuffer;
    }
    return std::to_string(SETTINGS.*(setting.valuePtr));
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
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});

  // LOCAL(feat): the tab bar is a top-level affordance; a sub-screen shows one
  // fixed category and reclaims that vertical space.
  if (!isSubScreen()) {
    buildTabBar(screen);
  } else {
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  }

  // rowItems_ (label/actionValue) was built by rebuildRowItems() when the
  // category was last selected/rebuilt; only the live value text needs
  // refreshing here, by assigning into the existing rowValues_ strings (no
  // vector growth) rather than building a new items/values vector on every
  // render.
  const auto& settings = *currentSettings;
  // Self-heal rather than index past the end: the loop below writes by row with
  // no bounds check, and a missed rebuild would corrupt the heap instead of just
  // drawing the wrong list.
  if (rowItems_.size() != settings.size() || rowValues_.size() != settings.size()) {
    rebuildRowItems();
  }
  for (size_t i = 0; i < settings.size(); i++) {
    rowValues_[i] = settingValueText(settings[i]);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  // maxLines=2 also marks the style explicitly set (an all-default smallText
  // fails textStyleUnset and the list would substitute bodyText back); the
  // common fits-on-one-line case takes the renderer's fast path anyway.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncTabListViewport(screen, props);
  screen.list(props);
}

void SettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  // LOCAL(feat): a sub-screen shows its group name and no version.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isSubScreen() ? I18N.get(subTitle_) : tr(STR_SETTINGS_TITLE), isSubScreen() ? "" : CROSSPOINT_VERSION);

  renderUi();

  // Top-level ring position 0 is the tab switcher (the hint shows the next
  // category name); otherwise the Confirm label reflects the focused row:
  // "Select" for pickers and actions, else "Toggle".
  const int ring = ringPos();
  const char* confirmLabel;
  if (!isSubScreen() && ring == 0) {
    confirmLabel = I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount]);
  } else {
    // LOCAL(feat): ACTION rows say "Select" too — upstream only special-cased
    // the sleep-timeout picker, so every sub-screen and action row read
    // "Toggle" even though Confirm opens something.
    const int sel = ring - 1;
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
