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

// Byte length of the dash separator starting at `i`, or 0 when there is none. En-dash
// (U+2013) and em-dash (U+2014) are E2 80 93/94 in UTF-8 and are always 3 bytes. A run of
// two or more ASCII hyphens is the typewriter em-dash ("word--word", the house style of
// Gutenberg-sourced EPUBs) and separates just the same; a SINGLE '-' must not, or
// "co-operate" would split in two and every line-break hyphen would stop merging with its
// continuation. Returning the length lets the caller step over a run of any width.
size_t dashLenAt(const char* text, size_t len, size_t i) {
  if (i + 2 < len && static_cast<uint8_t>(text[i]) == 0xE2 && static_cast<uint8_t>(text[i + 1]) == 0x80 &&
      (static_cast<uint8_t>(text[i + 2]) == 0x93 || static_cast<uint8_t>(text[i + 2]) == 0x94)) {
    return 3;
  }
  if (text[i] == '-' && i + 1 < len && text[i + 1] == '-') {
    size_t end = i + 2;
    while (end < len && text[end] == '-') end++;
    return end - i;
  }
  return 0;
}

}  // namespace

bool isSelectable(const char* text, size_t len, bool& outIsCjk) {
  outIsCjk = containsCjkLetter(text, len);
  return outIsCjk || containsAsciiAlnum(text, len);
}

size_t countDashes(const char* text, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len;) {
    const size_t d = dashLenAt(text, len, i);
    if (d > 0) {
      n++;
      i += d;
    } else {
      i++;
    }
  }
  return n;
}

size_t collectParts(const char* text, size_t len, Part* out, size_t maxParts) {
  if (!out || maxParts == 0 || len == 0) return 0;

  size_t n = 0;
  size_t partStart = 0;
  // The separator's own bytes belong to neither neighbour, so a part ends where its
  // separator begins. A stretch that is nothing but separators contributes no index.
  const auto emit = [&](size_t start, size_t end) {
    if (end <= start) return;
    if (n < maxParts) out[n++] = Part{static_cast<uint16_t>(start), static_cast<uint16_t>(end)};
  };

  for (size_t i = 0; i < len;) {
    const size_t d = dashLenAt(text, len, i);
    if (d > 0) {
      emit(partStart, i);
      i += d;
      partStart = i;
    } else {
      i++;
    }
  }
  emit(partStart, len);

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
