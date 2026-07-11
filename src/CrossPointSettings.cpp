#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <Serialization.h>

#include <cstring>
#include <mutex>
#include <string>

#include "I18nKeys.h"
#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void readAndValidate(HalFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 2;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";
constexpr char LANG_FILE_BIN[] = "/.crosspoint/language.bin";
constexpr char LANG_FILE_BAK[] = "/.crosspoint/language.bin.bak";

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
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
  std::lock_guard<std::mutex> lock(_mutex);
  // Apply the logging toggle live: every settings persist (device toggle, web API)
  // routes through here, so the master switch tracks the setting without a reboot.
  SdDebugLog::setMasterEnabled(sdCardLogging != 0);
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
  // Try JSON first
  if (Storage.exists(SETTINGS_FILE_JSON)) {
    String json = Storage.readFile(SETTINGS_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result;
      {
        std::lock_guard<std::mutex> lock(_mutex);
        result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
      }
      if (result && resave) {
        if (saveToFile()) {
          LOG_DBG("CPS", "Resaved settings to update format");
        } else {
          LOG_ERR("CPS", "Failed to resave settings after format update");
        }
      }
      migrateLanguageBinaryFile();
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      migrateLanguageBinaryFile();
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to settings.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save migrated settings to JSON");
        return false;
      }
    }
  }

  // No settings files at all -- check for standalone language.bin
  return migrateLanguageBinaryFile();
}

bool CrossPointSettings::migrateLanguageBinaryFile() {
  // V1_LANGUAGES / V1_LANGUAGE_COUNT are emitted by gen_i18n.py with the
  // frozen enum order from 2f969a9.
  if (!Storage.exists(LANG_FILE_BIN)) return false;

  HalFile f;
  if (Storage.openFileForRead("CPS", LANG_FILE_BIN, f)) {
    uint8_t version;
    serialization::readPod(f, version);
    if (version == 1) {
      uint8_t oldIndex;
      serialization::readPod(f, oldIndex);
      if (oldIndex < V1_LANGUAGE_COUNT) {
        language = static_cast<uint8_t>(V1_LANGUAGES[oldIndex]);
      }
    }
  }
  Storage.rename(LANG_FILE_BIN, LANG_FILE_BAK);
  saveToFile();
  LOG_DBG("CPS", "Migrated language.bin into settings.json");
  return true;
}

bool CrossPointSettings::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      uint8_t legacyFontFamily;
      serialization::readPod(inputFile, legacyFontFamily);
      if (legacyFontFamily < BUILTIN_FONT_COUNT) {
        fontFamily = legacyFontFamily;
      } else if (legacyFontFamily == LEGACY_OPENDYSLEXIC) {
        fontFamily = NOTOSERIF;
        strncpy(sdFontFamilyName, "OpenDyslexic", sizeof(sdFontFamilyName) - 1);
        sdFontFamilyName[sizeof(sdFontFamilyName) - 1] = '\0';
      }
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    uint8_t legacySleepTimeout = SLEEP_10_MIN;
    readAndValidate(inputFile, legacySleepTimeout, SLEEP_TIMEOUT_COUNT);
    sleepTimeoutMinutes = sleepTimeoutEnumToMinutes(legacySleepTimeout);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, longPressButtonBehavior, LONG_PRESS_BUTTON_BEHAVIOR_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, uiTheme);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, frontButtonFollowOrientation);
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  if (frontButtonMappingRead) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  LOG_DBG("CPS", "Settings loaded from binary file");
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
int CrossPointSettings::computeBuiltinFontId(const uint8_t family, const uint8_t size) {
  switch (family) {
    case NOTOSERIF:
    default:
      switch (size) {
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
      switch (size) {
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

int CrossPointSettings::getReaderFontId() const {
  if (readerOverride.active) {
    if (readerOverride.sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
      int id = sdFontIdResolver(sdFontResolverCtx, readerOverride.sdFontFamilyName, readerOverride.fontSize);
      if (id != 0) return id;
    }
    return computeBuiltinFontId(readerOverride.fontFamily, readerOverride.fontSize);
  }

  // Check SD card font first
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontSize);
    if (id != 0) return id;
    // Fall through to built-in if SD font not found
  }

  return computeBuiltinFontId(fontFamily, fontSize);
}

uint8_t CrossPointSettings::getReaderFontSize() const {
  return readerOverride.active ? readerOverride.fontSize : fontSize;
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
