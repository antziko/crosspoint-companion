#pragma once

#include <BoardConfig.h>
#include <HalClock.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "ReaderFontSizes.h"
#include "activities/settings/SettingsActivity.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

// Build the font size setting dynamically: the options are the point sizes the
// active family actually ships, so an SD family built at 10/12/14 offers three
// sizes and a family built at 8..18 offers six. The selected point size persists
// in SETTINGS.fontPointSize (saved/loaded manually in CrossPointSettings::toJson/
// fromJson — the generic loop skips dynamic entries), while the ENUM contract
// shared with the web UI stays index-based. Mirrors buildFontFamilySetting's
// s.dyn getter/setter pattern.
inline SettingInfo buildFontSizeSetting(const SdCardFontRegistry* registry) {
  // Captured by copy: getSettingsList() returns by value and the lambdas outlive
  // this call, so they must not reference the registry.
  const std::vector<uint8_t> sizes = readerFontPointSizes(registry, SETTINGS.sdFontFamilyName);

  // "pt" is deliberately not translated — see the matching note in
  // TextSettingsActivity::rebuildSizeList().
  std::vector<std::string> labels;
  labels.reserve(sizes.size());
  for (const uint8_t pt : sizes) {
    labels.push_back(std::to_string(pt) + " pt");
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_SIZE;
  s.type = SettingType::ENUM;
  s.enumStringValues = std::move(labels);
  s.key = "fontSize";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // matches the static font-size entry it replaces

  s.dyn = std::make_shared<SettingInfo::DynamicAccessors>();
  s.dyn->valueGetter = [sizes]() -> uint8_t {
    const uint8_t pt = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
    for (int i = 0; i < static_cast<int>(sizes.size()); i++) {
      if (sizes[i] == pt) return static_cast<uint8_t>(i);
    }
    return 0;
  };
  s.dyn->valueSetter = [sizes](uint8_t v) {
    if (v < sizes.size()) SETTINGS.fontPointSize = sizes[v];
  };

  return s;
}

// Build the font family setting dynamically. When registry is non-null, SD card fonts
// are appended after the built-in fonts. Otherwise only built-in fonts are listed.
inline SettingInfo buildFontFamilySetting(const SdCardFontRegistry* registry) {
  // Built-in font labels (StrId)
  std::vector<StrId> enumValues = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS};
  // Runtime string labels for SD card fonts
  std::vector<std::string> enumStringValues;

  // Reserve: first CrossPointSettings::BUILTIN_FONT_COUNT entries use StrId, rest use strings
  if (registry) {
    const auto& families = registry->getFamilies();
    enumStringValues.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(enumStringValues),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  // Capture the SD font count for the lambdas
  const int sdFontCount = static_cast<int>(enumStringValues.size());

  // Total option count = built-in + SD card families
  // For the combined enumStringValues: we need all entries as strings (built-in names + SD names)
  // The render code checks enumStringValues first, then enumValues. So we build enumStringValues
  // with all options when SD fonts are present.
  std::vector<std::string> allStringValues;
  if (sdFontCount > 0) {
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SERIF));
    allStringValues.push_back(I18N.get(StrId::STR_NOTO_SANS));
    allStringValues.insert(allStringValues.end(), enumStringValues.begin(), enumStringValues.end());
  }

  SettingInfo s;
  s.nameId = StrId::STR_FONT_FAMILY;
  s.type = SettingType::ENUM;
  s.enumValues = std::move(enumValues);
  s.enumStringValues = std::move(allStringValues);
  s.key = "fontFamily";
  s.category = StrId::STR_CAT_READER;
  s.inTextSettings = true;  // LOCAL(feat-dictionary): registry-aware font-family lives in Text Settings (#2605)

  // Capture registry families by copy for the lambdas
  std::vector<std::string> sdFamilyNames;
  if (registry) {
    const auto& families = registry->getFamilies();
    sdFamilyNames.reserve(families.size());
    std::transform(families.begin(), families.end(), std::back_inserter(sdFamilyNames),
                   [](const SdCardFontFamilyInfo& f) { return f.name; });
  }

  s.dyn = std::make_shared<SettingInfo::DynamicAccessors>();
  s.dyn->valueGetter = [sdFamilyNames]() -> uint8_t {
    // If an SD card font is selected, find its index
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      for (int i = 0; i < static_cast<int>(sdFamilyNames.size()); i++) {
        if (sdFamilyNames[i] == SETTINGS.sdFontFamilyName) {
          return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
        }
      }
      // SD font name not found in registry — fall through to built-in
    }
    return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  };

  s.dyn->valueSetter = [sdFamilyNames](uint8_t v) {
    if (v < CrossPointSettings::BUILTIN_FONT_COUNT) {
      SETTINGS.fontFamily = v;
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      int sdIdx = v - CrossPointSettings::BUILTIN_FONT_COUNT;
      if (sdIdx < static_cast<int>(sdFamilyNames.size())) {
        strncpy(SETTINGS.sdFontFamilyName, sdFamilyNames[sdIdx].c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
        SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      }
    }
  };

  return s;
}

// Build the dictionary selector setting dynamically, mirroring buildFontFamilySetting.
// Options are "None" (index 0) plus each installed dictionary's folder name. The choice
// is persisted in dictionary.bin (not the settings file) via Dictionary, so this entry
// uses a getter/setter and no valuePtr — the generic toJson/fromJson loop skips it. On device the entry is
// special-cased to open DictionarySelectActivity; on web it renders as a dropdown.
inline SettingInfo buildDictionarySetting(const DictionaryRegistry* registry) {
  std::vector<std::string> options;
  std::vector<std::string> basePaths;  // parallel to options[1..]; option index i -> basePaths[i-1]
  options.push_back(I18N.get(StrId::STR_DICT_NONE));
  if (registry) {
    const auto& entries = registry->getEntries();
    options.reserve(entries.size() + 1);
    basePaths.reserve(entries.size());
    for (const auto& e : entries) {
      options.push_back(e.name);
      basePaths.push_back(e.basePath);
    }
  }

  SettingInfo s;
  s.nameId = StrId::STR_DICTIONARY;
  s.type = SettingType::ENUM;
  s.enumStringValues = std::move(options);
  s.key = "dictionary";
  s.category = StrId::STR_READER_DICTIONARY;

  s.dyn = std::make_shared<SettingInfo::DynamicAccessors>();
  s.dyn->valueGetter = [basePaths]() -> uint8_t {
    const std::string active = Dictionary::readDictPath(nullptr);
    if (active.empty()) return 0;
    for (size_t i = 0; i < basePaths.size(); i++) {
      if (basePaths[i] == active) return static_cast<uint8_t>(i + 1);
    }
    return 0;  // active dictionary no longer installed -> show "None"
  };

  s.dyn->valueSetter = [basePaths](uint8_t v) {
    if (v == 0 || v > basePaths.size()) {
      Dictionary::saveGlobalDictPath("");  // clear selection
    } else {
      Dictionary::saveGlobalDictPath(basePaths[v - 1].c_str());
    }
  };

  return s;
}

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The list is rebuilt on each call rather than cached in a static, so its ~10-12 KB of
// SettingInfo entries is not held resident while reading (where RAM is tightest) — it only
// lives for the duration of a settings load/save/render. When an SdCardFontRegistry with SD
// fonts (or a DictionaryRegistry) is supplied, the font-family / dictionary entries are built
// in their registry-aware form.
inline std::vector<SettingInfo> getSettingsList(const SdCardFontRegistry* registry = nullptr,
                                                const DictionaryRegistry* dictRegistry = nullptr) {
  // Built fresh on each call (not a static cache) so the ~10-12 KB of SettingInfo entries is not
  // held resident while reading — it lives only for the duration of a settings load/save/render,
  // all of which are off the render loop. Callers are infrequent (boot load, settings change, web
  // settings, settings-screen open). Use push_back, not brace-init, to avoid placing all ~56
  // SettingInfo temporaries on the stack at once via std::initializer_list, which would overflow
  // the 8 KB loopTask stack; push_back keeps peak stack usage to ~sizeof(SettingInfo).
  std::vector<SettingInfo> v;
  v.reserve(60);

  // --- Display ---
  // Appearance basics stay at the Display top level; sleep-screen and e-ink refresh tuning live in
  // their two sub-screens (categories STR_DISP_SLEEP / STR_DISP_EINK).
  // Enum settings are persisted as their numeric value, and SettingInfo::Enum maps the picker index
  // straight to that value — so a label list must be ordered by enum value, not by menu preference.
  // Bind each label to its SLEEP_SCREEN_MODE value explicitly so an enum reorder (as #2635 did to
  // COVER_CUSTOM/BLANK) can never again silently swap two modes' labels. (#2644)
  std::vector<StrId> sleepScreenValues(CrossPointSettings::SLEEP_SCREEN_MODE_COUNT);
  sleepScreenValues[CrossPointSettings::DARK] = StrId::STR_DARK;
  sleepScreenValues[CrossPointSettings::LIGHT] = StrId::STR_LIGHT;
  sleepScreenValues[CrossPointSettings::CUSTOM] = StrId::STR_CUSTOM;
  sleepScreenValues[CrossPointSettings::COVER] = StrId::STR_COVER;
  sleepScreenValues[CrossPointSettings::COVER_CUSTOM] = StrId::STR_COVER_CUSTOM;
  sleepScreenValues[CrossPointSettings::BLANK] = StrId::STR_NONE_OPT;
  sleepScreenValues[CrossPointSettings::QUICK_RESUME] = StrId::STR_QUICK_RESUME;
  v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen, std::move(sleepScreenValues),
                                "sleepScreen", StrId::STR_DISP_SLEEP));
  v.push_back(SettingInfo::Toggle(StrId::STR_SLEEP_REVIEW_ON_WAKE, &CrossPointSettings::reviewSleepImageOnWake,
                                  "reviewSleepImageOnWake", StrId::STR_DISP_SLEEP));
  v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                                {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_DISP_SLEEP));
  v.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                                {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                                "sleepScreenCoverFilter", StrId::STR_DISP_SLEEP));
  v.push_back(SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                                {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen",
                                StrId::STR_DISP_SLEEP));
  v.push_back(
      SettingInfo::Enum(StrId::STR_IMAGE_DITHER, &CrossPointSettings::imageDither,
                        {StrId::STR_DITHER_BLUE_NOISE, StrId::STR_DITHER_BAYER, StrId::STR_DITHER_ERROR_DIFFUSION},
                        "imageDither", StrId::STR_DISP_EINK));
  v.push_back(SettingInfo::Enum(StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
                                {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15,
                                 StrId::STR_PAGES_30, StrId::STR_PAGES_60, StrId::STR_PAGES_NEVER},
                                "refreshFrequency", StrId::STR_DISP_EINK));
  v.push_back(SettingInfo::Enum(StrId::STR_REFRESH_ACTION, &CrossPointSettings::refreshAction,
                                {StrId::STR_REFRESH_ACTION_FULL, StrId::STR_REFRESH_ACTION_BW_REINFORCEMENT},
                                "refreshAction", StrId::STR_DISP_EINK));
  v.push_back(
      SettingInfo::Enum(StrId::STR_REFRESH_SCREEN_MODE, &CrossPointSettings::refreshScreenMode,
                        {StrId::STR_REFRESH_MODE_FAST, StrId::STR_REFRESH_MODE_HALF, StrId::STR_REFRESH_MODE_FULL},
                        "refreshScreenMode", StrId::STR_DISP_EINK));
  v.push_back(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                                  StrId::STR_DISP_EINK));
  v.push_back(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                                {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                                StrId::STR_CAT_DISPLAY));
  v.push_back(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                                {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                                 StrId::STR_THEME_ROUNDEDRAFF, StrId::STR_THEME_VEGA},
                                "uiTheme", StrId::STR_CAT_DISPLAY));
  v.push_back(
      SettingInfo::Enum(StrId::STR_DISPLAY_ORIENTATION, &CrossPointSettings::displayOrientation,
                        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                        "displayOrientation", StrId::STR_CAT_DISPLAY));

  // --- Reader ---
  // Built-in font-family entry. Replaced per-call with a registry-aware version when SD fonts installed.
  v.push_back(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                                {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS}, "fontFamily", StrId::STR_CAT_READER)
                  .withTextSettings());
  // Placeholder: the selectable sizes depend on the active font family, so this
  // entry is always replaced by buildFontSizeSetting() below. It only fixes the
  // setting's position in the Reader category.
  v.push_back(
      SettingInfo::Enum(StrId::STR_FONT_SIZE, nullptr, {}, "fontSize", StrId::STR_CAT_READER).withTextSettings());
  v.push_back(SettingInfo::Enum(StrId::STR_DICT_FONT_FAMILY, &CrossPointSettings::dictionaryFontFamily,
                                {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS}, "dictionaryFontFamily",
                                StrId::STR_READER_DICTIONARY));
  v.push_back(SettingInfo::Enum(StrId::STR_DICT_FONT_SIZE, &CrossPointSettings::dictionaryFontSize,
                                {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE},
                                "dictionaryFontSize", StrId::STR_READER_DICTIONARY));
  v.push_back(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                                {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing",
                                StrId::STR_CAT_READER)
                  .withTextSettings());
  v.push_back(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin,
                                 {CrossPointSettings::SCREEN_MARGIN_MIN, CrossPointSettings::SCREEN_MARGIN_MAX,
                                  CrossPointSettings::SCREEN_MARGIN_STEP},
                                 "screenMargin", StrId::STR_CAT_READER)
                  .withTextSettings());
  v.push_back(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                                {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                 StrId::STR_BOOK_S_STYLE},
                                "paragraphAlignment", StrId::STR_CAT_READER)
                  .withTextSettings());
  v.push_back(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                                  StrId::STR_READER_TEXT)
                  .withTextSettings());
  v.push_back(SettingInfo::Toggle(StrId::STR_FOCUS_READING, &CrossPointSettings::focusReadingEnabled,
                                  "focusReadingEnabled", StrId::STR_READER_TEXT)
                  .withTextSettings());
  v.push_back(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                                  StrId::STR_READER_TEXT)
                  .withTextSettings());
  v.push_back(
      SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                        "orientation", StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                                  "extraParagraphSpacing", StrId::STR_READER_TEXT)
                  .withTextSettings());
  v.push_back(SettingInfo::Enum(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing,
                                {StrId::STR_TEXT_AA_OFF, StrId::STR_TEXT_AA_ANTIALIASED, StrId::STR_TEXT_AA_SHARP},
                                "textAntiAliasing", StrId::STR_READER_TEXT)
                  .withTextSettings());
  v.push_back(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                                {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                                "imageRendering", StrId::STR_READER_TEXT));
  // --- Reader > Dictionary sub-group (category STR_READER_DICTIONARY) ---
  // The dictionary selector is special-cased on Confirm (opens DictionarySelectActivity); the
  // marker-by-dwell options are a device-only action row added in SettingsActivity. Placeholder
  // dictionary entry (no installed dictionaries); replaced per-call with a registry-aware version.
  v.push_back(buildDictionarySetting(nullptr));
  v.push_back(SettingInfo::Value(
      StrId::STR_LOOKUP_HIST_CAP, &CrossPointSettings::lookupHistoryCap,
      {CrossPointSettings::HIST_CAP_MIN, CrossPointSettings::HIST_CAP_UNLIMITED, CrossPointSettings::HIST_CAP_STEP},
      "lookupHistoryCap", StrId::STR_READER_DICTIONARY));
  // flashcardCardStyle + flashcardSessionScope are NOT registered here: both are
  // chosen per-session on the FlashcardReviewActivity overview (Up/Down = card
  // style, Left/Right = due-first / shuffled) and the picks are persisted as the
  // defaults. Hand-persisted in CrossPointSettings::toJson/fromJson (like dictMarker).
  v.push_back(SettingInfo::Enum(StrId::STR_HOLD_CONFIRM, &CrossPointSettings::holdConfirmAction,
                                {StrId::STR_STATE_OFF, StrId::STR_HOLD_CONFIRM_BOOKMARK, StrId::STR_HOLD_CONFIRM_DICT},
                                "holdConfirmAction", StrId::STR_READER_DICTIONARY));
  // --- Reader > Reading Tracking sub-group (category STR_READER_TRACKING) ---
  v.push_back(SettingInfo::Enum(
      StrId::STR_MIN_SESSION_FOR_STATS, &CrossPointSettings::minSessionMinutes,
      {StrId::STR_ALWAYS, StrId::STR_SEC_15, StrId::STR_SEC_30, StrId::STR_MIN_1, StrId::STR_MIN_2, StrId::STR_MIN_5},
      "minSessionMinutes", StrId::STR_READER_TRACKING));
  v.push_back(SettingInfo::Enum(
      StrId::STR_IDLE_PAGE_CAP, &CrossPointSettings::pageIdleCapSeconds,
      {StrId::STR_STATE_OFF, StrId::STR_SEC_15, StrId::STR_SEC_30, StrId::STR_SEC_45, StrId::STR_SEC_60},
      "pageIdleCapSeconds", StrId::STR_READER_TRACKING));
  v.push_back(SettingInfo::Enum(
      StrId::STR_PROGRESS_SAVE_INTERVAL, &CrossPointSettings::progressSaveIntervalIdx,
      {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
      "progressSaveIntervalIdx", StrId::STR_READER_TRACKING));

  // --- Controls ---
  // Grouped by physical button family so related options sit together:
  //   Front buttons -> Side buttons -> Power -> Tilt.
  // The RemapFrontButtons / RemapFrontButtonsCW ACTION rows are inserted ahead of
  // these by SettingsActivity, so the front-button group leads the Controls screen.

  // Front buttons
  v.push_back(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION,
                                  &CrossPointSettings::frontButtonFollowOrientation, "frontButtonFollowOrientation",
                                  StrId::STR_CAT_CONTROLS));
  v.push_back(
      SettingInfo::Enum(StrId::STR_FRONT_LONG_PRESS_BEHAVIOR, &CrossPointSettings::longPressButtonBehavior,
                        {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                         StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION, StrId::STR_LONG_PRESS_BEHAVIOR_BOOKMARK_SYNC},
                        "longPressButtonBehavior", StrId::STR_CAT_CONTROLS));

  // Side buttons
  v.push_back(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                                {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV, StrId::STR_DISABLED}, "sideButtonLayout",
                                StrId::STR_CAT_CONTROLS));
  // Touch reader controls (#2481). Filtered out below on non-touch boards.
  v.push_back(SettingInfo::Enum(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                                {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "touchReaderControls",
                                StrId::STR_CAT_CONTROLS));
  v.push_back(SettingInfo::Toggle(StrId::STR_SWAP_SIDE_BTN_CW, &CrossPointSettings::swapSideButtonsCW,
                                  "swapSideButtonsCW", StrId::STR_CAT_CONTROLS));
  v.push_back(
      SettingInfo::Enum(StrId::STR_SIDE_LONG_PRESS_BEHAVIOR, &CrossPointSettings::sideLongPressButtonBehavior,
                        {StrId::STR_LONG_PRESS_BEHAVIOR_OFF, StrId::STR_LONG_PRESS_BEHAVIOR_SKIP,
                         StrId::STR_LONG_PRESS_BEHAVIOR_ORIENTATION, StrId::STR_LONG_PRESS_BEHAVIOR_BOOKMARK_SYNC},
                        "sideLongPressButtonBehavior", StrId::STR_CAT_CONTROLS));

  // Power button (tiltPageTurn is inserted right after this row further below)
  v.push_back(SettingInfo::Enum(
      StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
      {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_FOOTNOTES},
      "shortPwrBtn", StrId::STR_CAT_CONTROLS));
  v.push_back(SettingInfo::Toggle(StrId::STR_PWR_BTN_FOOTNOTE_BACK, &CrossPointSettings::pwrBtnFootnoteBack,
                                  "pwrBtnFootnoteBack", StrId::STR_CAT_CONTROLS));
  v.push_back(SettingInfo::Toggle(StrId::STR_BACK_SHORT_TO_FILE_BROWSER, &CrossPointSettings::backShortToFileBrowser,
                                  "backShortToFileBrowser", StrId::STR_CAT_CONTROLS));

  // --- System ---
  v.push_back(SettingInfo::Value(
      StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeoutMinutes,
      {CrossPointSettings::MIN_SLEEP_TIMEOUT_MINUTES, CrossPointSettings::MAX_SLEEP_TIMEOUT_MINUTES, 1},
      "sleepTimeoutMinutes", StrId::STR_CAT_SYSTEM));
  // --- System > Sync Prompts sub-group (category STR_SYS_SYNC_PROMPTS) ---
  // KOReader sync-before-sleep / sync-before-reading prompts: master toggles + shared
  // reading-minutes-since-last-sync threshold.
  v.push_back(SettingInfo::Toggle(StrId::STR_SYNC_PROMPT_ON_SLEEP, &CrossPointSettings::syncPromptOnSleep,
                                  "syncPromptOnSleep", StrId::STR_SYS_SYNC_PROMPTS));
  v.push_back(SettingInfo::Toggle(StrId::STR_SYNC_PROMPT_ON_OPEN, &CrossPointSettings::syncPromptOnOpen,
                                  "syncPromptOnOpen", StrId::STR_SYS_SYNC_PROMPTS));
  v.push_back(SettingInfo::Enum(StrId::STR_SYNC_PROMPT_MINUTES, &CrossPointSettings::syncPromptMinutesIdx,
                                {StrId::STR_MIN_3, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15,
                                 StrId::STR_MIN_20, StrId::STR_MIN_25, StrId::STR_MIN_30},
                                "syncPromptMinutesIdx", StrId::STR_SYS_SYNC_PROMPTS));
  // --- System > Library & Storage sub-group (category STR_SYS_LIBRARY; + Clear Cache action) ---
  v.push_back(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                                  StrId::STR_SYS_LIBRARY));
  v.push_back(SettingInfo::Toggle(StrId::STR_REMOVE_READ_FROM_RECENTS, &CrossPointSettings::removeReadBooksFromRecents,
                                  "removeReadBooksFromRecents", StrId::STR_SYS_LIBRARY));
  v.push_back(SettingInfo::Toggle(StrId::STR_MOVE_FINISHED_TO_READ, &CrossPointSettings::moveFinishedToReadFolder,
                                  "moveFinishedToReadFolder", StrId::STR_SYS_LIBRARY));
  v.push_back(SettingInfo::Toggle(StrId::STR_SD_LOGGING, &CrossPointSettings::sdCardLogging, "sdCardLogging",
                                  StrId::STR_SYS_LIBRARY));
  // OPDS download filename format: persisted + web-exposed, category-less so it is hidden
  // from the on-device Settings screen (cycled from the OPDS server list). Downloads use
  // this branch's per-server folder model, so no global download-folder setting is added. (#2571)
  v.push_back(SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                                {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                                "opdsFilenameFormat"));

  // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
      [](const std::string& v) {
        KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
      [](const std::string& v) {
        KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
        KOREADER_STORE.saveToFile();
      },
      "koPassword", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
      [](const std::string& v) {
        KOREADER_STORE.setServerUrl(v);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
  // Send document metadata with progress sync (#1820); per-server, applies to the active server.
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata()); },
      [](uint8_t v) {
        KOREADER_STORE.setSendMetadata(v != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSendMetadata", StrId::STR_KOREADER_SYNC));
  // Sync behavior (#2192); per-server, applies to the active server. Order matches the enum:
  // 0 = Ask every time, 1 = Smart sync.
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
      [](uint8_t v) {
        KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
        KOREADER_STORE.saveToFile();
      },
      "koSyncBehavior", StrId::STR_KOREADER_SYNC));

  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
  v.push_back(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                                  "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                  &CrossPointSettings::statusBarBookProgressPercentage,
                                  "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                                {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                                StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(
      SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                        {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                        "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                                {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                                StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                                {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP}, "xtcStatusBarMode",
                                StrId::STR_CUSTOMISE_STATUS_BAR));
  // Clock entries (web settings only; device UI uses ClockOffsetActivity for the offset).
  // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
  v.push_back(SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, "statusBarClock",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                                 "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                                {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                                StrId::STR_CUSTOMISE_STATUS_BAR));
  // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
  // on next WiFi connect, which is useful when crossing time zones.
  v.push_back(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced,
                                  "clockHasBeenSynced", StrId::STR_CUSTOMISE_STATUS_BAR));
  // Date display (needs a clock: X3 DS3231 RTC, or X4 NTP over WiFi)
  v.push_back(SettingInfo::Toggle(StrId::STR_DATE, &CrossPointSettings::statusBarDate, "statusBarDate",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(
      SettingInfo::Enum(StrId::STR_DATE_FORMAT, &CrossPointSettings::dateFormat,
                        {StrId::STR_DATE_FMT_0, StrId::STR_DATE_FMT_1, StrId::STR_DATE_FMT_2, StrId::STR_DATE_FMT_3},
                        "dateFormat", StrId::STR_CUSTOMISE_STATUS_BAR));
  // Home top bar clock/date (independent from reader status bar)
  v.push_back(SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::homeTopBarClock, "homeTopBarClock",
                                  StrId::STR_CUSTOMISE_TOP_BAR));
  v.push_back(SettingInfo::Toggle(StrId::STR_DATE, &CrossPointSettings::homeTopBarDate, "homeTopBarDate",
                                  StrId::STR_CUSTOMISE_TOP_BAR));
  v.push_back(
      SettingInfo::Enum(StrId::STR_DATE_FORMAT, &CrossPointSettings::homeTopBarDateFormat,
                        {StrId::STR_DATE_FMT_0, StrId::STR_DATE_FMT_1, StrId::STR_DATE_FMT_2, StrId::STR_DATE_FMT_3},
                        "homeTopBarDateFormat", StrId::STR_CUSTOMISE_TOP_BAR));
  // Only show tilt page turn setting when the QMI8658 IMU is present (X3)
  if (halTiltSensor.isAvailable()) {
    // Insert after the short power button setting (end of Controls section)
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->nameId == StrId::STR_SHORT_PWR_BTN) {
        v.insert(it + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                           {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                           "tiltPageTurn", StrId::STR_CAT_CONTROLS));
        break;
      }
    }
  }

  if (dictRegistry) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_DICTIONARY; });
    if (it != v.end()) {
      *it = buildDictionarySetting(dictRegistry);
    }
  }
  if (registry && registry->getFamilyCount() > 0) {
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_FAMILY; });
    if (it != v.end()) {
      *it = buildFontFamilySetting(registry);
    }
  }
  {
    // Unconditional: even with no SD fonts installed the sizes come from the
    // built-in family rather than a fixed Small/Medium/Large/XL enum.
    auto it = std::find_if(v.begin(), v.end(), [](const SettingInfo& s) { return s.nameId == StrId::STR_FONT_SIZE; });
    if (it != v.end()) {
      *it = buildFontSizeSetting(registry);
    }
  }

  // Touch-vs-button setting visibility (#2481, corrected per #2689). Non-touch
  // boards (X3/X4) hide the touch-only control; touch boards hide the front-
  // button orientation follow and the sunlight fading fix (frontlight devices).
  if (!BoardConfig::hasTouch()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) { return s.nameId == StrId::STR_TOUCH_READER_CONTROLS; }),
            v.end());
  } else {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) {
                             return s.nameId == StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION ||
                                    s.nameId == StrId::STR_SUNLIGHT_FADING_FIX;
                           }),
            v.end());
  }
  return v;
}
