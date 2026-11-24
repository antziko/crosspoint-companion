#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <string>

// The language Translate Page asks Gemini for (from matcha-reader 272ae112). Persisted by index in
// CrossPointSettings::translationTarget: append only.
namespace translation {

struct TargetLanguage {
  StrId label;
  const char* promptName;  // the English name the request uses
};
constexpr TargetLanguage TARGET_LANGUAGES[] = {
    {StrId::STR_XLAT_LANG_EN, "English"},
    {StrId::STR_XLAT_LANG_DE, "German"},
    {StrId::STR_XLAT_LANG_FR, "French"},
    {StrId::STR_XLAT_LANG_ES, "Spanish"},
    {StrId::STR_XLAT_LANG_IT, "Italian"},
    {StrId::STR_XLAT_LANG_PT, "Portuguese"},
    {StrId::STR_XLAT_LANG_NL, "Dutch"},
    {StrId::STR_XLAT_LANG_HU, "Hungarian"},
    {StrId::STR_XLAT_LANG_PL, "Polish"},
    {StrId::STR_XLAT_LANG_RU, "Russian"},
    {StrId::STR_XLAT_LANG_UK, "Ukrainian"},
    {StrId::STR_XLAT_LANG_JA, "Japanese"},
    {StrId::STR_XLAT_LANG_ZH, "Simplified Chinese"},
    {StrId::STR_XLAT_LANG_KO, "Korean"},
};
constexpr uint8_t TARGET_LANGUAGE_COUNT = sizeof(TARGET_LANGUAGES) / sizeof(TARGET_LANGUAGES[0]);
// The translationTarget value meaning "the language written in gemini.lang".
constexpr uint8_t TARGET_CUSTOM = TARGET_LANGUAGE_COUNT;
constexpr size_t CUSTOM_LANGUAGE_MAX = 40;

// The language name for the request. Custom reads gemini.lang (next to gemini.key); with no file,
// or an empty one, the request asks for English.
std::string targetLanguageName(uint8_t index);

}  // namespace translation
