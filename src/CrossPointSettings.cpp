#include "CrossPointSettings.h"

#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <SdDebugLog.h>

#include <cstring>
#include <iterator>
#include <string>

#include "I18nKeys.h"
#include "ReaderFontSizes.h"
#include "SettingsList.h"
#include "fontIds.h"

namespace {

// Longest "<key>_obf" JSON key we ever build on the stack; SettingsList keys are
// far shorter, so this avoids a per-field std::string allocation during load.
constexpr size_t OBF_KEY_BUF = 64;

void copyToField(char* dest, const char* src, const size_t maxLen) {
  strncpy(dest, src, maxLen - 1);
  dest[maxLen - 1] = '\0';
}

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

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
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
  doc["fontFamily"] = s.fontFamily;
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

  // Flashcard style + session scope — chosen on the review overview, not in SettingsList.
  doc["flashcardCardStyle"] = s.flashcardCardStyle;
  doc["flashcardSessionScope"] = s.flashcardSessionScope;

  // Language — managed by LanguageSelectActivity, not in SettingsList. Stored as ISO code
  // string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";
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

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      // destPtr starts out holding the struct-initializer default; it stays that
      // way unless the document actually carries a value for this key.
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        needsResave = true;
        continue;
      }

      bool loaded = false;
      if (info.obfuscated) {
        char obfKey[OBF_KEY_BUF];
        snprintf(obfKey, sizeof(obfKey), "%s_obf", info.key);
        bool ok = false;
        bool tooLong = false;
        const std::string decoded =
            obfuscation::deobfuscateFromBase64(doc[obfKey] | "", info.stringMaxLen - 1, &ok, &tooLong);
        if (tooLong) {
          LOG_ERR("CPS", "Oversized obfuscated value for key '%s'", info.key);
          needsResave = true;
        }
        if (ok && !decoded.empty()) {
          copyToField(destPtr, decoded.c_str(), info.stringMaxLen);
          loaded = true;
        }
      }
      if (!loaded) {
        // Read as const char*, never `| std::string(...)`: ArduinoJson's
        // std::string converter drags a per-TU copy of the serializer into
        // flash. See the note in PersistableStore.h.
        const char* raw = doc[info.key].is<const char*>() ? doc[info.key].as<const char*>() : nullptr;
        if (raw) {
          // Obfuscated field recovered from a legacy plaintext value -> resave.
          if (info.obfuscated && strcmp(raw, destPtr) != 0) needsResave = true;
          copyToField(destPtr, raw, info.stringMaxLen);
        }
      }
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before overwrite
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumValues.size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
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

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | (uint8_t)0;
  s.fontFamily = clamp(storedFontFamily, BUILTIN_FONT_COUNT, 0);
  // SD card font family name — not in SettingsList, load manually.
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  // Pinned-font set — newline-separated keys, load manually.
  const char* pf = doc["pinnedFonts"] | "";
  strncpy(s.pinnedFonts, pf, sizeof(s.pinnedFonts) - 1);
  s.pinnedFonts[sizeof(s.pinnedFonts) - 1] = '\0';
  if (storedFontFamily == LEGACY_OPENDYSLEXIC && s.sdFontFamilyName[0] == '\0') {
    s.fontFamily = NOTOSERIF;
    strncpy(s.sdFontFamilyName, "OpenDyslexic", sizeof(s.sdFontFamilyName) - 1);
    s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
    needsResave = true;
  } else if (storedFontFamily >= BUILTIN_FONT_COUNT) {
    needsResave = true;
  }

  // Dictionary marker-by-dwell — not in SettingsList (device-only sub-screen), load manually.
  constexpr uint8_t kDictMarkerT1Count = sizeof(S::DICT_MARKER_T1_SECONDS) / sizeof(uint16_t);
  constexpr uint8_t kDictMarkerT2Count = sizeof(S::DICT_MARKER_T2_SECONDS) / sizeof(uint16_t);
  s.dictMarkerDwellEnabled =
      clamp(doc["dictMarkerDwellEnabled"] | s.dictMarkerDwellEnabled, 2, s.dictMarkerDwellEnabled);
  s.dictMarkerT1Idx = clamp(doc["dictMarkerT1Idx"] | s.dictMarkerT1Idx, kDictMarkerT1Count, s.dictMarkerT1Idx);
  s.dictMarkerT2Idx = clamp(doc["dictMarkerT2Idx"] | s.dictMarkerT2Idx, kDictMarkerT2Count, s.dictMarkerT2Idx);

  // Flashcard style + session scope — chosen on the review overview, not in SettingsList.
  s.flashcardCardStyle =
      clamp(doc["flashcardCardStyle"] | s.flashcardCardStyle, S::FLASHCARD_CARD_STYLE_COUNT, s.flashcardCardStyle);
  s.flashcardSessionScope = clamp(doc["flashcardSessionScope"] | s.flashcardSessionScope,
                                  S::FLASHCARD_SESSION_SCOPE_COUNT, s.flashcardSessionScope);

  // Language — stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    s.language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  if (needsResave) {
    LOG_DBG("CPS", "Resaving settings to update format");
    requestResave();
  }

  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}

// static
float CrossPointSettings::computeLineCompression(const uint8_t family, const uint8_t lineSpacing,
                                                 const char* sdFontName) {
  // SD card fonts use same compression as Bookerly (the most neutral values)
  if (sdFontName && sdFontName[0] != '\0') {
    switch (lineSpacing) {
      case TIGHT:
        return 0.95f;
      case NORMAL:
      default:
        return 1.0f;
      case WIDE:
        return 1.1f;
    }
  }

  switch (family) {
    case NOTOSERIF:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

float CrossPointSettings::getReaderLineCompression() const {
  if (readerOverride.active) {
    return computeLineCompression(readerOverride.fontFamily, readerOverride.lineSpacing,
                                  readerOverride.sdFontFamilyName);
  }
  return computeLineCompression(fontFamily, lineSpacing, sdFontFamilyName);
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

int CrossPointSettings::getDefinitionFontId() const {
  const FONT_FAMILY effFamily = static_cast<FONT_FAMILY>(dictionaryFontFamily);
  const FONT_SIZE effSize = static_cast<FONT_SIZE>(dictionaryFontSize);
  switch (effFamily) {
    case NOTOSERIF:
    default:
      switch (effSize) {
        case SMALL:
          return NOTOSERIF_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSERIF_14_FONT_ID;
        case LARGE:
          return NOTOSERIF_16_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSERIF_18_FONT_ID;
      }
    case NOTOSANS:
      switch (effSize) {
        case SMALL:
          return NOTOSANS_12_FONT_ID;
        case MEDIUM:
        default:
          return NOTOSANS_14_FONT_ID;
        case LARGE:
          return NOTOSANS_16_FONT_ID;
        case EXTRA_LARGE:
          return NOTOSANS_18_FONT_ID;
      }
  }
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
  const FONT_FAMILY effFamily = static_cast<FONT_FAMILY>(dictionaryFontFamily);
  switch (effFamily) {
    case NOTOSERIF:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case NOTOSANS:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

// static
// `pointSize` is an actual reader point size. A built-in family only exists at
// BUILTIN_READER_POINT_SIZES, so a size carried over from an SD family is snapped
// to the nearest built-in size first (allocation-free — this runs in the render loop).
int CrossPointSettings::computeBuiltinFontId(const uint8_t family, const uint8_t pointSize) {
  const uint8_t pt =
      snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), pointSize);
  const bool sans = (family == NOTOSANS);
  switch (pt) {
    case 12:
      return sans ? NOTOSANS_12_FONT_ID : NOTOSERIF_12_FONT_ID;
    case 16:
      return sans ? NOTOSANS_16_FONT_ID : NOTOSERIF_16_FONT_ID;
    case 18:
      return sans ? NOTOSANS_18_FONT_ID : NOTOSERIF_18_FONT_ID;
    case 14:
    default:
      return sans ? NOTOSANS_14_FONT_ID : NOTOSERIF_14_FONT_ID;
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

uint8_t CrossPointSettings::getReaderExtraParagraphSpacing() const {
  return readerOverride.active ? readerOverride.extraParagraphSpacing : extraParagraphSpacing;
}
