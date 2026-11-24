#include "TranslationTarget.h"

#include <HalStorage.h>
#include <SdSystemDir.h>

namespace translation {

namespace {
bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string readCustomLanguage() {
  std::string path;
  if (!sdsystem::findExistingUserFile("gemini.lang", path)) return "";
  char buf[64];  // CUSTOM_LANGUAGE_MAX plus room for whitespace and a BOM
  size_t len = Storage.readFileToBuffer(path.c_str(), buf, sizeof(buf));
  size_t start = 0;
  if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF && static_cast<unsigned char>(buf[1]) == 0xBB &&
      static_cast<unsigned char>(buf[2]) == 0xBF) {
    start = 3;
  }
  while (start < len && isSpace(buf[start])) start++;
  while (len > start && isSpace(buf[len - 1])) len--;
  if (len - start > CUSTOM_LANGUAGE_MAX) len = start + CUSTOM_LANGUAGE_MAX;
  return std::string(buf + start, len - start);
}
}  // namespace

std::string targetLanguageName(const uint8_t index) {
  if (index == TARGET_CUSTOM) {
    std::string custom = readCustomLanguage();
    return custom.empty() ? TARGET_LANGUAGES[0].promptName : custom;
  }
  return TARGET_LANGUAGES[index < TARGET_LANGUAGE_COUNT ? index : 0].promptName;
}

}  // namespace translation
