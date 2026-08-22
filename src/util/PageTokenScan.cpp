#include "PageTokenScan.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace PageTokens {
namespace {

// Soft-hyphen U+00AD, 2 UTF-8 bytes.
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

// True when the token carries at least one CJK letter (Han, Kana, Hangul, fullwidth
// letters/digits) — i.e. content rather than CJK punctuation. utf8IsCjkBreakable is the
// same predicate layout uses to split CJK runs (ParsedText.cpp:399), so what it accepts
// here is exactly what arrives as one-character tokens.
bool containsCjkLetter(const char* text, size_t len) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text);
  const auto* end = ptr + len;
  while (ptr < end) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp) && !utf8IsCjkPunctuation(cp)) return true;
  }
  return false;
}

// Paired with containsCjkLetter. Kept a pure byte test (it only ever matches ASCII), so the
// two together read as "carries a letter or digit in either script family".
bool containsAsciiAlnum(const char* text, size_t len) {
  return std::any_of(text, text + len, [](unsigned char c) { return c < 0x80 && std::isalnum(c) != 0; });
}

bool isDashAt(const char* text, size_t len, size_t i) {
  return i + 2 < len && static_cast<uint8_t>(text[i]) == 0xE2 && static_cast<uint8_t>(text[i + 1]) == 0x80 &&
         (static_cast<uint8_t>(text[i + 2]) == 0x93 || static_cast<uint8_t>(text[i + 2]) == 0x94);
}

}  // namespace

bool isSelectable(const char* text, size_t len, bool& outIsCjk) {
  outIsCjk = containsCjkLetter(text, len);
  return outIsCjk || containsAsciiAlnum(text, len);
}

size_t countDashes(const char* text, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i + 2 < len; i++) {
    if (isDashAt(text, len, i)) {
      n++;
      i += 2;
    }
  }
  return n;
}

size_t collectParts(const char* text, size_t len, Part* out, size_t maxParts) {
  if (!out || maxParts == 0 || len == 0) return 0;

  size_t n = 0;
  // A part's end is the next part's start, so emission always lags one start behind.
  size_t prevStart = 0;
  bool havePrev = false;
  size_t partStart = 0;

  const auto emit = [&](size_t start, size_t end) {
    // The dash bytes sit at the tail of the part before them; they are a separator, not text.
    while (end >= start + 3 && isDashAt(text, len, end - 3)) end -= 3;
    if (end <= start) return;  // a part that was nothing but dashes contributes no index
    if (n < maxParts) out[n++] = Part{static_cast<uint16_t>(start), static_cast<uint16_t>(end)};
  };
  const auto pushStart = [&](size_t start) {
    if (havePrev) emit(prevStart, start);
    prevStart = start;
    havePrev = true;
  };

  for (size_t i = 0; i < len;) {
    if (isDashAt(text, len, i)) {
      if (i > partStart) pushStart(partStart);
      i += 3;
      partStart = i;
    } else {
      i++;
    }
  }
  if (partStart < len) pushStart(partStart);
  if (havePrev) emit(prevStart, len);

  return n;
}

int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const std::string& word, EpdFontFamily::Style style) {
  if (word.find(SOFT_HYPHEN_UTF8) == std::string::npos) {
    return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.c_str(), style));
  }
  std::string sanitized = word;
  size_t pos = 0;
  while ((pos = sanitized.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    sanitized.erase(pos, SOFT_HYPHEN_BYTES);
  }
  return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, sanitized.c_str(), style));
}

int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style) {
  if (!text) return 0;
  if (!std::strstr(text, SOFT_HYPHEN_UTF8)) {
    return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, text, style));  // no copy
  }
  return measureAdvance(renderer, fontId, std::string(text), style);
}

int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const char* text, size_t len,
                       EpdFontFamily::Style style) {
  return measureAdvance(renderer, fontId, std::string(text, len), style);
}

}  // namespace PageTokens
