#include "CrossPointSettings.h"

#include <BoardConfig.h>
#include <I18n.h>
#include <Logging.h>
#include <SdDebugLog.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "I18nKeys.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SettingsPersistence.h"
#include "fontIds.h"
#include "util/PluginLocations.h"

namespace {

// Migrate a pre-refactor settings file that used the single combined `statusBar`
// enum into the current per-element status bar fields. Runs when the new
// statusBarChapterPageCount key is absent (see fromJson()).
void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::NONE:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }
}

}  // namespace

namespace {
// Reset a logical->hardware quad to factory order if any two roles collide.
void validateFrontButtonQuad(uint8_t& back, uint8_t& confirm, uint8_t& left, uint8_t& right) {
  const uint8_t mapping[] = {back, confirm, left, right};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        back = CrossPointSettings::FRONT_HW_BACK;
        confirm = CrossPointSettings::FRONT_HW_CONFIRM;
        left = CrossPointSettings::FRONT_HW_LEFT;
        right = CrossPointSettings::FRONT_HW_RIGHT;
        return;
      }
    }
  }
}
}  // namespace

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  validateFrontButtonQuad(settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                          settings.frontButtonRight);
  validateFrontButtonQuad(settings.frontButtonBackCW, settings.frontButtonConfirmCW, settings.frontButtonLeftCW,
                          settings.frontButtonRightCW);
}

uint8_t CrossPointSettings::sleepTimeoutEnumToMinutes(const uint8_t legacyValue) {
  switch (legacyValue) {
    case SLEEP_1_MIN:
      return 1;
    case SLEEP_5_MIN:
      return 5;
    case SLEEP_15_MIN:
      return 15;
    case SLEEP_30_MIN:
      return 30;
    case SLEEP_10_MIN:
    default:
      return 10;
  }
}

bool CrossPointSettings::saveToFile() const {
  // Apply the logging toggle live on every persist (device toggle, web API) so the
  // SD-debug master switch tracks the setting without a reboot, then delegate the
  // actual JSON write (and /.crosspoint creation) to the PersistableStore base.
  SdDebugLog::setMasterEnabled(sdCardLogging != 0);
  return PersistableStore<CrossPointSettings>::saveToFile();
}

void CrossPointSettings::toJson(JsonDocument& doc) const {
  const CrossPointSettings& s = *this;

  // Walks the constexpr table in SettingsPersistence.h rather than getSettingsList(), which
  // builds ~5.8 KB of SettingInfo on the heap — a contiguous allocation that aborts (and so
  // reboots) when a save lands at low heap. See the header for the full rationale.
  for (const PersistedU8& p : kPersistedSettings) {
    doc[p.key] = s.*(p.ptr);
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  // LandscapeCW front button override — managed by RemapFrontButtonsCW sub-activity.
  doc["frontButtonBackCW"] = s.frontButtonBackCW;
  doc["frontButtonConfirmCW"] = s.frontButtonConfirmCW;
  doc["frontButtonLeftCW"] = s.frontButtonLeftCW;
  doc["frontButtonRightCW"] = s.frontButtonRightCW;
  // Font family and size — both use dynamic getter/setters in SettingsList (the
  // option lists depend on the SD font registry), so the generic loop skips them.
  doc["fontFamily"] = STORED_NOTOSANS;
  doc["fontSize"] = s.fontPointSize;
  // SD card font family name — not in SettingsList, save manually.
  if (s.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = s.sdFontFamilyName;
  }
  // Pinned-font set (Font Family picker) — newline-separated keys, save manually.
  if (s.pinnedFonts[0] != '\0') {
    doc["pinnedFonts"] = s.pinnedFonts;
  }

  // Dictionary marker-by-dwell — device-only (inline rows in the Reader > Dictionary
  // sub-screen), persisted here by explicit fields rather than a SettingInfo key, so the
  // generic loop doesn't see it. Persist manually.
  doc["dictMarkerDwellEnabled"] = s.dictMarkerDwellEnabled;
  doc["dictMarkerT1Idx"] = s.dictMarkerT1Idx;
  doc["dictMarkerT2Idx"] = s.dictMarkerT2Idx;
  doc["dictInlineGlossEnabled"] = s.dictInlineGlossEnabled;

  // Flashcard style + session scope — chosen on the review overview, not in SettingsList.
  doc["flashcardCardStyle"] = s.flashcardCardStyle;
  doc["flashcardSessionScope"] = s.flashcardSessionScope;

  // Language — managed by LanguageSelectActivity, not in SettingsList. Stored as ISO code
  // string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";

  // A uint16_t mask, so it does not fit the uint8_t generic loop. Omitted while
  // unconfigured, so the default keeps following the UI language.
  if (s.keyboardLayouts != 0) {
    doc["keyboardLayouts"] = s.keyboardLayouts;
  }

  // A uint32_t epoch, likewise outside the uint8_t table. Omitted until a sync has
  // actually happened, so an untouched file stays free of a meaningless zero.
  if (s.clockLastSyncEpoch != 0) {
    doc["clockLastSyncEpoch"] = s.clockLastSyncEpoch;
  }
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  CrossPointSettings& s = *this;
  bool needsResave = false;

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // Legacy migration: if statusBarChapterPageCount is absent this is a pre-refactor settings
  // file. Populate s with migrated values now so the generic loop below picks them up as
  // defaults and clamps them.
  if (doc["statusBarChapterPageCount"].isNull()) {
    applyLegacyStatusBarSettings(s);
  }

  // Mirrors toJson(): walks the constexpr table, not getSettingsList(). The clamps below
  // reproduce what the SettingInfo loop applied per SettingType — VALUE sliders clamp into
  // their range, TOGGLE/ENUM fall back to the field's struct-initializer default, which is
  // what keeps sentinel values (minSessionMinutes' MIN_SESSION_USE_GLOBAL) intact instead of
  // folding them onto a bound.
  for (const PersistedU8& p : kPersistedSettings) {
    const uint8_t fieldDefault = s.*(p.ptr);  // struct-initializer default, read before overwrite
    uint8_t v = doc[p.key] | fieldDefault;
    if (p.clampToRange) {
      if (v < p.lo)
        v = p.lo;
      else if (v > p.hi)
        v = p.hi;
    } else if (v < p.lo || v > p.hi) {
      v = fieldDefault;
    }
    s.*(p.ptr) = v;
  }

  // Plugin System did not exist in older settings files. Preserve the behavior
  // of users who already have SD plugins installed, while keeping the new
  // default disabled on clean installs.
  if (doc["pluginsEnabled"].isNull()) {
    s.pluginsEnabled = PluginLocations::anyPluginInstalledOnDisk() ? 1 : 0;
    needsResave = true;
  }

  // Older files stored one combined touch mode under "touchReaderControls":
  // 0=off, 1=tap, 2=swipe, 3=inverted tap. Split it into the master toggle plus the
  // per-direction gesture pair. Read from the raw document, not the member: the loop
  // above has already folded a legacy 2 or 3 back to the On default, since the enum
  // now only reaches 1.
  if (doc["pageTurnGesture"].isNull() && doc["previousPageGesture"].isNull() &&
      doc["touchReaderControls"].is<uint8_t>()) {
    const uint8_t mode = doc["touchReaderControls"].as<uint8_t>();
    if (mode >= 1 && mode <= 3) {
      s.touchReaderControls = TOUCH_READER_ON;
      s.pageTurnGesture = mode == 1 ? TAP_ONLY : mode == 2 ? SWIPE_ONLY : INVERTED_TAP;
      s.previousPageGesture = s.pageTurnGesture;
      needsResave = true;
    }
  }

  // Line spacing is a percent step under "lineHeight". The older named-step keys were renamed at
  // each renumbering, so the newest key present identifies the numbering; remap it once.
  if (doc["lineHeight"].isNull()) {
    static constexpr struct {
      const char* key;
      uint8_t steps;
    } kLegacyLineSpacing[] = {{"lineSpacingV3", 6}, {"lineSpacingV2", 5}, {"lineSpacing", 4}};
    for (const auto& legacy : kLegacyLineSpacing) {
      if (doc[legacy.key].is<uint8_t>()) {
        s.lineSpacing = remapLegacyLineSpacing(doc[legacy.key].as<uint8_t>(), legacy.steps);
        needsResave = true;
        break;
      }
    }
  }

  // Paragraph spacing was an on/off toggle under the old key; On was today's Normal gap.
  if (doc["paragraphSpacing"].isNull() && !doc["extraParagraphSpacing"].isNull()) {
    s.paragraphSpacing = (doc["extraParagraphSpacing"] | (uint8_t)1) ? PARA_SPACING_NORMAL : PARA_SPACING_OFF;
    needsResave = true;
  }

  // refreshScreenMode was FAST/HALF/FULL. FAST cleared nothing, so it folds onto HALF.
  if (doc["refreshScreenModeV2"].isNull() && !doc["refreshScreenMode"].isNull()) {
    s.refreshScreenMode = (doc["refreshScreenMode"] | (uint8_t)1) == 2 ? RSM_FULL : RSM_HALF;
    needsResave = true;
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)SLEEP_10_MIN, SLEEP_TIMEOUT_COUNT, (uint8_t)SLEEP_10_MIN);
    s.sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacyValue);
    needsResave = true;
  }

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  using S = CrossPointSettings;
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  // LandscapeCW front button override — default to factory order when absent (older files).
  s.frontButtonBackCW =
      clamp(doc["frontButtonBackCW"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirmCW = clamp(doc["frontButtonConfirmCW"] | (uint8_t)S::FRONT_HW_CONFIRM,
                                 S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_CONFIRM);
  s.frontButtonLeftCW =
      clamp(doc["frontButtonLeftCW"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRightCW =
      clamp(doc["frontButtonRightCW"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  validateFrontButtonMapping(s);

  // Reader font size — an actual point size since 1.5. Files written by 1.4 and
  // earlier hold the old SMALL/MEDIUM/LARGE/EXTRA_LARGE slot in 0..3; no font is
  // renderable at those sizes, so the range is unambiguous and folds to the
  // point sizes those slots used to mean. Drop this once 1.4 upgrades are done.
  uint8_t storedFontSize = doc["fontSize"] | DEFAULT_FONT_POINT_SIZE;
  if (storedFontSize <= LEGACY_FONT_SIZE_MAX) {
    storedFontSize = 12 + storedFontSize * 2;  // 0,1,2,3 -> 12,14,16,18
    needsResave = true;
  }
  fontPointSize = storedFontSize;
  // Dictionary font size: the Small/Medium/Large/XL slot under "dictionaryFontSize" became a
  // point size under "dictionaryFontPointSize"; fold the slot the same way as above.
  if (doc["dictionaryFontPointSize"].isNull() && !doc["dictionaryFontSize"].isNull()) {
    const uint8_t slot = doc["dictionaryFontSize"] | (uint8_t)1;
    if (slot <= LEGACY_FONT_SIZE_MAX) s.dictionaryFontPointSize = 12 + slot * 2;
    needsResave = true;
  }

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | STORED_NOTOSANS;
  s.fontFamily = NOTOSANS;
  // SD card font family name — not in SettingsList, load manually.

  // The Home key's long press used to run holdConfirmAction. Carry that choice into the
  // new per-gesture setting once, so an X4 Pro keeps the hold it was configured with.
  // holdConfirmAction itself stays: it still owns the Confirm hold on boards without a
  // Home key, and carries REFRESH_SCREEN, which has no Home-key equivalent.
  if (BoardConfig::hasHomeKey() && doc["homeButtonLongPressAction"].isNull() && !doc["holdConfirmAction"].isNull()) {
    static constexpr HomeButtonAction LEGACY[] = {
        HomeButtonAction::Ignore,     HomeButtonAction::Bookmark, HomeButtonAction::Dictionary, HomeButtonAction::Sync,
        HomeButtonAction::ReaderMenu, HomeButtonAction::Refresh,  HomeButtonAction::Footnotes};
    static_assert(sizeof(LEGACY) / sizeof(LEGACY[0]) == HOLD_CONFIRM_ACTION_COUNT);
    if (s.holdConfirmAction < HOLD_CONFIRM_ACTION_COUNT) {
      s.homeButtonLongPressAction = static_cast<uint8_t>(LEGACY[s.holdConfirmAction]);
      needsResave = true;
    }
  }
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  // Pinned-font set — newline-separated keys, load manually.
  const char* pf = doc["pinnedFonts"] | "";
  strncpy(s.pinnedFonts, pf, sizeof(s.pinnedFonts) - 1);
  s.pinnedFonts[sizeof(s.pinnedFonts) - 1] = '\0';
  // The former built-in Noto Serif and OpenDyslexic are SD families now.
  if (s.sdFontFamilyName[0] == '\0') {
    const char* legacyFamily = storedFontFamily == LEGACY_NOTOSERIF      ? "NotoSerif"
                               : storedFontFamily == LEGACY_OPENDYSLEXIC ? "OpenDyslexic"
                                                                         : "";
    strncpy(s.sdFontFamilyName, legacyFamily, sizeof(s.sdFontFamilyName) - 1);
    s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  }
  if (storedFontFamily != STORED_NOTOSANS) needsResave = true;
  // Dictionary font: the old key stored 0 Noto Serif, 1 Noto Sans, 2 Same as book.
  if (doc["dictionaryFontFamilyV2"].isNull() && doc["dictionaryFontFamily"].is<uint8_t>()) {
    s.dictionaryFontFamily = doc["dictionaryFontFamily"].as<uint8_t>() == 2 ? DICT_FONT_MATCH_READER : NOTOSANS;
    needsResave = true;
  }

  // Dictionary marker-by-dwell — not in SettingsList (device-only sub-screen), load manually.
  constexpr uint8_t kDictMarkerT1Count = sizeof(S::DICT_MARKER_T1_SECONDS) / sizeof(uint16_t);
  constexpr uint8_t kDictMarkerT2Count = sizeof(S::DICT_MARKER_T2_SECONDS) / sizeof(uint16_t);
  s.dictMarkerDwellEnabled =
      clamp(doc["dictMarkerDwellEnabled"] | s.dictMarkerDwellEnabled, 2, s.dictMarkerDwellEnabled);
  s.dictMarkerT1Idx = clamp(doc["dictMarkerT1Idx"] | s.dictMarkerT1Idx, kDictMarkerT1Count, s.dictMarkerT1Idx);
  s.dictMarkerT2Idx = clamp(doc["dictMarkerT2Idx"] | s.dictMarkerT2Idx, kDictMarkerT2Count, s.dictMarkerT2Idx);
  s.dictInlineGlossEnabled =
      clamp(doc["dictInlineGlossEnabled"] | s.dictInlineGlossEnabled, 2, s.dictInlineGlossEnabled);

  // Flashcard style + session scope — chosen on the review overview, not in SettingsList.
  s.flashcardCardStyle =
      clamp(doc["flashcardCardStyle"] | s.flashcardCardStyle, S::FLASHCARD_CARD_STYLE_COUNT, s.flashcardCardStyle);
  s.flashcardSessionScope = clamp(doc["flashcardSessionScope"] | s.flashcardSessionScope,
                                  S::FLASHCARD_SESSION_SCOPE_COUNT, s.flashcardSessionScope);

  // Language — stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    s.language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  // Absent means unconfigured, which is the default.
  if (doc["keyboardLayouts"].is<uint16_t>()) {
    keyboardLayouts = doc["keyboardLayouts"].as<uint16_t>();
  }

  if (doc["clockLastSyncEpoch"].is<uint32_t>()) {
    clockLastSyncEpoch = doc["clockLastSyncEpoch"].as<uint32_t>();
  }

  if (needsResave) {
    LOG_DBG("CPS", "Resaving settings to update format");
    requestResave();
  }

  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}

// static
uint8_t CrossPointSettings::remapLegacyLineSpacing(const uint8_t legacy, const uint8_t legacySteps) {
  // Tight/Normal/Wide/Extra Wide.
  static constexpr uint8_t V1[] = {LINE_SPACING_110, LINE_SPACING_120, LINE_SPACING_160, LINE_SPACING_175};
  // Semi-Wide inserted at index 2.
  static constexpr uint8_t V2[] = {LINE_SPACING_110, LINE_SPACING_120, LINE_SPACING_145, LINE_SPACING_160,
                                   LINE_SPACING_175};
  // Relaxed inserted at index 2.
  static constexpr uint8_t V3[] = {LINE_SPACING_110, LINE_SPACING_120, LINE_SPACING_130,
                                   LINE_SPACING_145, LINE_SPACING_160, LINE_SPACING_175};
  const uint8_t* table = legacySteps == 4 ? V1 : legacySteps == 5 ? V2 : V3;
  return legacy < legacySteps && legacySteps <= std::size(V3) ? table[legacy] : static_cast<uint8_t>(LINE_SPACING_120);
}

// static
float CrossPointSettings::lineSpacingMultiplier(const uint8_t lineSpacing) {
  return LINE_SPACING_MULTIPLIERS[lineSpacing < LINE_COMPRESSION_COUNT ? lineSpacing : LINE_SPACING_120];
}

float CrossPointSettings::getReaderLineCompression() const {
  return lineSpacingMultiplier(readerOverride.active ? readerOverride.lineSpacing : lineSpacing);
}

bool CrossPointSettings::isFontPinned(const char* key) const {
  if (!key || key[0] == '\0') return false;
  const size_t keyLen = strlen(key);
  // pinnedFonts is newline-separated; match a full line (between separators).
  const char* p = pinnedFonts;
  while (*p) {
    const char* nl = strchr(p, '\n');
    const size_t lineLen = nl ? static_cast<size_t>(nl - p) : strlen(p);
    if (lineLen == keyLen && strncmp(p, key, keyLen) == 0) return true;
    if (!nl) break;
    p = nl + 1;
  }
  return false;
}

void CrossPointSettings::setFontPinned(const char* key, const bool pinned) {
  if (!key || key[0] == '\0') return;
  const bool already = isFontPinned(key);
  if (pinned == already) return;

  if (pinned) {
    // Append "key\n". Bounds-check against the fixed buffer; silently skip if full.
    const size_t cur = strlen(pinnedFonts);
    const size_t keyLen = strlen(key);
    if (cur + keyLen + 2 > sizeof(pinnedFonts)) return;  // +1 '\n' +1 '\0'
    memcpy(pinnedFonts + cur, key, keyLen);
    pinnedFonts[cur + keyLen] = '\n';
    pinnedFonts[cur + keyLen + 1] = '\0';
    return;
  }

  // Remove: rebuild the buffer omitting the matching line.
  const size_t keyLen = strlen(key);
  char rebuilt[sizeof(pinnedFonts)];
  size_t w = 0;
  const char* p = pinnedFonts;
  while (*p) {
    const char* nl = strchr(p, '\n');
    const size_t lineLen = nl ? static_cast<size_t>(nl - p) : strlen(p);
    const bool isMatch = (lineLen == keyLen && strncmp(p, key, keyLen) == 0);
    if (!isMatch && lineLen > 0) {
      memcpy(rebuilt + w, p, lineLen);
      w += lineLen;
      rebuilt[w++] = '\n';
    }
    if (!nl) break;
    p = nl + 1;
  }
  rebuilt[w] = '\0';
  memcpy(pinnedFonts, rebuilt, w + 1);
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  if (sleepTimeoutMinutes >= SLEEP_TIMEOUT_NEVER_MINUTES) return 0UL;
  const uint8_t minutes =
      std::clamp(sleepTimeoutMinutes, MIN_SLEEP_TIMEOUT_MINUTES, static_cast<uint8_t>(SLEEP_TIMEOUT_NEVER_MINUTES - 1));
  return static_cast<unsigned long>(minutes) * 60UL * 1000UL;
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
    case REFRESH_60:
      return 60;
    case REFRESH_NEVER:
      return REFRESH_COUNTDOWN_DISABLED;
  }
}

int CrossPointSettings::getNightRefreshFrequency() const {
  switch (nightRefreshFrequency) {
    case NIGHT_REFRESH_1:
      return 1;
    case NIGHT_REFRESH_3:
      return 3;
    case NIGHT_REFRESH_5:
      return 5;
    case NIGHT_REFRESH_10:
      return 10;
    case NIGHT_REFRESH_15:
      return 15;
    case NIGHT_REFRESH_SAME:
    default:
      return 0;
  }
}

int CrossPointSettings::getEffectiveRefreshFrequency() const {
  const int base = getRefreshFrequency();
  // REFRESH_COUNTDOWN_DISABLED ("Never") is negative and must stay exactly that.
  if (!screenInverted || base <= 0) return base;
  const int night = getNightRefreshFrequency();
  return night > 0 ? night : base;
}

uint8_t CrossPointSettings::getDefinitionPointSize() const {
  const uint8_t pt = readerOverride.active && readerOverride.dictFontPointSize != ReaderOverride::DICT_FONT_SIZE_GLOBAL
                         ? readerOverride.dictFontPointSize
                         : dictionaryFontPointSize;
  return std::clamp(pt, DICT_FONT_POINT_MIN, DICT_FONT_POINT_MAX);
}

int CrossPointSettings::getDefinitionFontId() const {
  // "Same as book": the FAMILY follows the reader (including the per-book override and
  // any SD family), but the SIZE stays the dictionary's own setting — a definition read
  // at the book's 18pt wastes most of the screen.
  //
  // Allocation-free by contract: this runs inside layout/render loops, so it only ever
  // LOOKS UP a font. Making the SD family resident at the dictionary's point size is
  // DictionaryDefinitionActivity::onEnter's job (SdCardFontSystem::ensureFontSize); until
  // that has run, the resolver returns the reader-size font and definitions simply render
  // at the book's size rather than failing.
  if (dictionaryFontFamily == DICT_FONT_MATCH_READER) {
    const uint8_t pointSize = getDefinitionPointSize();
    const char* sdFamily = getReaderSdFontFamilyName();
    if (sdFamily[0] != '\0' && sdFontIdResolver) {
      const int id = sdFontIdResolver(sdFontResolverCtx, sdFamily, pointSize);
      if (id != 0) return id;
    }
    return computeBuiltinFontId(readerOverride.active ? readerOverride.fontFamily : fontFamily, pointSize);
  }

  // A built-in dictionary family exists only at BUILTIN_READER_POINT_SIZES; this snaps to them.
  return computeBuiltinFontId(dictionaryFontFamily, getDefinitionPointSize());
}

void CrossPointSettings::clearSdFontFamily() {
  sdFontFamilyName[0] = '\0';
  fontPointSize =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), fontPointSize);
  saveToFile();
}

int CrossPointSettings::getReaderFontId() const {
  // Per-book override takes precedence when active.
  if (readerOverride.active) {
    if (readerOverride.sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
      int id = sdFontIdResolver(sdFontResolverCtx, readerOverride.sdFontFamilyName, readerOverride.fontPointSize);
      if (id != 0) return id;
    }
    return computeBuiltinFontId(readerOverride.fontFamily, readerOverride.fontPointSize);
  }

  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontPointSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  return computeBuiltinFontId(fontFamily, fontPointSize);
}

float CrossPointSettings::getDefinitionLineCompression() const {
  uint8_t spacing = readerOverride.active ? readerOverride.lineSpacing : lineSpacing;
  if (readerOverride.active && readerOverride.dictLineSpacing < LINE_COMPRESSION_COUNT) {
    spacing = readerOverride.dictLineSpacing;
  }
  return lineSpacingMultiplier(spacing);
}

// static
// `pointSize` is an actual reader point size. A built-in family only exists at
// BUILTIN_READER_POINT_SIZES, so a size carried over from an SD family is snapped
// to the nearest built-in size first (allocation-free — this runs in the render loop).
int CrossPointSettings::computeBuiltinFontId(const uint8_t family, const uint8_t pointSize) {
  const uint8_t pt =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), pointSize);
  (void)family;
  switch (pt) {
    case 12:
      return NOTOSANS_12_FONT_ID;
    case 16:
      return NOTOSANS_16_FONT_ID;
    case 18:
      return NOTOSANS_18_FONT_ID;
    case 14:
    default:
      return NOTOSANS_14_FONT_ID;
  }
}

uint8_t CrossPointSettings::getReaderFontSize() const {
  return readerOverride.active ? readerOverride.fontPointSize : fontPointSize;
}

uint8_t CrossPointSettings::getReaderScreenMargin() const {
  return readerOverride.active ? readerOverride.screenMargin : screenMargin;
}

const char* CrossPointSettings::getReaderSdFontFamilyName() const {
  return readerOverride.active ? readerOverride.sdFontFamilyName : sdFontFamilyName;
}

void CrossPointSettings::setReaderOverride(const ReaderOverride& ov) { readerOverride = ov; }

void CrossPointSettings::clearReaderOverride() {
  readerOverride = ReaderOverride{};  // resets active = false + all fields to defaults
}

uint8_t CrossPointSettings::getReaderParagraphAlignment() const {
  return readerOverride.active ? readerOverride.paragraphAlignment : paragraphAlignment;
}

uint8_t CrossPointSettings::getReaderHyphenationEnabled() const {
  return readerOverride.active ? readerOverride.hyphenationEnabled : hyphenationEnabled;
}

uint8_t CrossPointSettings::getReaderParagraphSpacing() const {
  return readerOverride.active ? readerOverride.paragraphSpacing : paragraphSpacing;
}

uint8_t CrossPointSettings::getReaderParagraphIndent() const {
  return readerOverride.active ? readerOverride.paragraphIndent : paragraphIndent;
}

uint8_t CrossPointSettings::getReaderWordSpacing() const {
  return readerOverride.active ? readerOverride.wordSpacing : wordSpacing;
}

int8_t CrossPointSettings::getDefinitionCharacterSpacing() const {
  uint8_t v = dictCharacterSpacing;
  if (readerOverride.active && readerOverride.dictCharacterSpacing <= CHARACTER_SPACING_MAX) {
    v = readerOverride.dictCharacterSpacing;
  }
  return static_cast<int8_t>(std::min(v, CHARACTER_SPACING_MAX) - CHARACTER_SPACING_OFFSET);
}

int8_t CrossPointSettings::getReaderCharacterSpacing() const {
  const uint8_t v = readerOverride.active ? readerOverride.characterSpacing : characterSpacing;
  return static_cast<int8_t>(v - CHARACTER_SPACING_OFFSET);
}

int8_t CrossPointSettings::getReaderTextWeight() const {
  const uint8_t v = readerOverride.active ? readerOverride.textWeight : textWeight;
  return static_cast<int8_t>(v - TEXT_WEIGHT_OFFSET);
}

uint8_t CrossPointSettings::getReaderEmbeddedStyle() const {
  return readerOverride.active ? readerOverride.embeddedStyle : embeddedStyle;
}

uint8_t CrossPointSettings::getReaderTextAntiAliasing() const {
  return readerOverride.active ? readerOverride.textAntiAliasing : textAntiAliasing;
}

uint8_t CrossPointSettings::getReaderFocusReading() const {
  return readerOverride.active ? readerOverride.focusReadingEnabled : focusReadingEnabled;
}

uint8_t CrossPointSettings::getReaderInlineGloss() const {
  return readerOverride.active ? readerOverride.inlineGlossEnabled : dictInlineGlossEnabled;
}

ReaderRenderSpec CrossPointSettings::readerRenderSpec(const uint16_t viewportWidth,
                                                      const uint16_t viewportHeight) const {
  ReaderRenderSpec spec;
  // Every field with a per-book counterpart goes through its getReader* accessor.
  // Reading the raw member here is what silently invalidates a cache: the load path
  // compares against the override, so a build that wrote the global value can never
  // match and the section is rebuilt on every open.
  spec.fontId = getReaderFontId();
  spec.lineCompression = getReaderLineCompression();
  spec.paragraphSpacing = getReaderParagraphSpacing();
  spec.paragraphIndent = getReaderParagraphIndent();
  spec.characterSpacing = getReaderCharacterSpacing();
  spec.wordSpacingPercent = getReaderWordSpacing();
  spec.paragraphAlignment = getReaderParagraphAlignment();
  spec.viewportWidth = viewportWidth;
  spec.viewportHeight = viewportHeight;
  spec.hyphenationEnabled = getReaderHyphenationEnabled() != 0;
  spec.embeddedStyle = getReaderEmbeddedStyle() != 0;
  // No per-book counterpart: the global value is the resolved one.
  // Same layout as Display, so switching between them keeps the section cache.
  spec.imageRendering = imageRendering == IMAGES_DISPLAY_BW ? static_cast<uint8_t>(IMAGES_DISPLAY) : imageRendering;
  spec.focusReadingEnabled = getReaderFocusReading() != 0;
  spec.imageBleed = getReaderScreenMargin();
  return spec;
}

bool CrossPointSettings::getReaderSwapWordSelectAxes() const {
  return readerOverride.active && readerOverride.swapWordSelectAxes != 0;
}
