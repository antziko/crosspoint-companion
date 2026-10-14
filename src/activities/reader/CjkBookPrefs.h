#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <string>

#include "util/CjkScript.h"

// Per-book CJK reading state, kept in the book cache as cjk.bin. Separate from
// reader_settings.bin so the matcha-ported vertical/furigana features stay out of that
// versioned layout.
struct CjkBookPrefs {
  // cjk::LanguageChoice: LANG_AUTO follows the tag (or the sniff), the rest re-tag the book.
  uint8_t languageOverride = cjk::LANG_AUTO;
  // What the content sniff found when the tag named no CJK language: 0 = not yet sniffed,
  // a LanguageChoice, or SNIFFED_NONE for a Latin book (so it is not sniffed again).
  uint8_t detectedLanguage = 0;
  // -1 = auto, 0 = off, 1 = on.
  int8_t verticalOverride = -1;
  int8_t furiganaOverride = -1;

  static constexpr uint8_t SNIFFED_NONE = 0xFF;
  bool operator==(const CjkBookPrefs&) const = default;

  // The language the book is read as: the override, else the tag when it names a CJK language,
  // else what the sniff found, else the tag as written (possibly empty).
  std::string effectiveLanguage(const std::string& tag) const {
    if (const char* forced = cjk::languageTagForChoice(languageOverride)) return forced;
    if (cjk::isUnscriptedChinese(tag)) {
      // Plain "zh": the text decides between the two scripts.
      return detectedLanguage == cjk::LANG_ZH_HANT ? cjk::languageTagForChoice(detectedLanguage) : tag;
    }
    if (cjk::scriptForLanguage(tag) != CjkScript::None) return tag;
    if (const char* sniffed = cjk::languageTagForChoice(detectedLanguage)) return sniffed;
    return tag;
  }

  bool furiganaOn() const { return furiganaOverride != 0; }

  // Vertical (tategaki) only ever applies to a CJK book. Auto: Japanese is vertical; Chinese
  // follows the publisher's page progression.
  bool verticalOn(const CjkScript script, const bool spineRtl) const {
    if (script == CjkScript::None) return false;
    if (verticalOverride >= 0) return verticalOverride == 1;
    return script == CjkScript::Japanese || (cjk::isChinese(script) && spineRtl);
  }

  static std::string path(const std::string& cachePath) { return cachePath + "/cjk.bin"; }

  bool load(const std::string& cachePath) {
    *this = CjkBookPrefs{};
    HalFile f;
    if (!Storage.exists(path(cachePath).c_str())) return false;
    if (!Storage.openFileForRead("CJK", path(cachePath), f)) return false;
    uint8_t data[4] = {0, 0, 0xFF, 0xFF};
    if (f.read(data, sizeof(data)) < 1) return false;
    if (data[0] < cjk::LANGUAGE_CHOICE_COUNT) languageOverride = data[0];
    if (data[1] < cjk::LANGUAGE_CHOICE_COUNT || data[1] == SNIFFED_NONE) detectedLanguage = data[1];
    const auto tri = [](const uint8_t b) -> int8_t { return b <= 1 ? static_cast<int8_t>(b) : -1; };
    verticalOverride = tri(data[2]);
    furiganaOverride = tri(data[3]);
    return true;
  }

  bool save(const std::string& cachePath) const {
    HalFile f;
    if (!Storage.openFileForWrite("CJK", path(cachePath), f)) {
      LOG_ERR("CJK", "Cannot save cjk.bin");
      return false;
    }
    const uint8_t data[4] = {languageOverride, detectedLanguage, static_cast<uint8_t>(verticalOverride),
                             static_cast<uint8_t>(furiganaOverride)};
    return f.write(data, sizeof(data)) == sizeof(data);
  }
};
