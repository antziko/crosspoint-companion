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

namespace {
// Opening quotes and brackets: they lead the character after them.
bool isOpeningMark(const uint32_t cp) {
  switch (cp) {
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0x301D:  // 〝
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

// End of the part starting at `from`: the next dash, or the next CJK letter once the part holds
// one, taking any opening marks before it along. Layout gives CJK one token per character but
// glues punctuation to both neighbours, so 础”“出 arrives as one token; it splits as 础” and “出.
size_t partEnd(const char* text, const size_t len, const size_t from) {
  bool hasCjkLetter = false;
  size_t openStart = len;
  for (size_t i = from; i < len;) {
    if (dashLenAt(text, len, i) > 0) return i;
    const auto* p = reinterpret_cast<const unsigned char*>(text + i);
    const uint32_t cp = utf8NextCodepoint(&p);
    const size_t next = std::min(len, static_cast<size_t>(reinterpret_cast<const char*>(p) - text));
    if (cp == 0 || next <= i) return len;
    if (utf8IsCjkBreakable(cp) && !utf8IsCjkPunctuation(cp)) {
      if (hasCjkLetter) return openStart < len ? openStart : i;
      hasCjkLetter = true;
      openStart = len;
    } else if (isOpeningMark(cp)) {
      if (openStart == len) openStart = i;
    } else {
      openStart = len;
    }
    i = next;
  }
  return len;
}
}  // namespace

size_t countDashes(const char* text, size_t len) {
  size_t n = 0;
  for (size_t i = 0; i < len;) {
    const size_t d = dashLenAt(text, len, i);
    if (d > 0) {
      n++;
      i += d;
      continue;
    }
    i = partEnd(text, len, i);
    if (i < len && dashLenAt(text, len, i) == 0) n++;  // a CJK split
  }
  return n;
}

namespace {
// Byte length of the separator at text[pos], or 0. U+2000-U+200A (except no-break U+2007) and
// U+205F count: the chapter parser splits words on them, and snippets saved before it did
// still carry them inside a word.
size_t spaceLenAt(const char* text, const size_t pos) {
  const char c = text[pos];
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return 1;
  if (static_cast<uint8_t>(c) != 0xE2 || text[pos + 1] == '\0' || text[pos + 2] == '\0') return 0;
  const auto b1 = static_cast<uint8_t>(text[pos + 1]);
  const auto b2 = static_cast<uint8_t>(text[pos + 2]);
  if (b1 == 0x80 && b2 >= 0x80 && b2 <= 0x8A && b2 != 0x87) return 3;
  if (b1 == 0x81 && b2 == 0x9F) return 3;
  return 0;
}
}  // namespace

bool nextTextPart(const char* text, size_t from, size_t& outStart, size_t& outLen, size_t& outNext) {
  if (!text) return false;
  size_t pos = from;
  for (;;) {
    while (const size_t sp = spaceLenAt(text, pos)) pos += sp;
    if (text[pos] == '\0') return false;
    size_t end = pos;
    while (text[end] != '\0' && spaceLenAt(text, end) == 0) end++;

    bool isCjk = false;
    if (isSelectable(text + pos, end - pos, isCjk)) {
      // Separators belong to neither neighbour, so skip any leading run before measuring
      // the part -- which is also how collectParts drops them.
      while (pos < end) {
        const size_t d = dashLenAt(text, end, pos);
        if (d == 0) break;
        pos += d;
      }
      if (pos < end) {
        const size_t stop = partEnd(text, end, pos);
        outStart = pos;
        outLen = stop - pos;
        outNext = stop;
        return true;
      }
    }
    pos = end;
  }
}

size_t collectParts(const char* text, size_t len, Part* out, size_t maxParts) {
  if (!out || maxParts == 0 || len == 0) return 0;

  size_t n = 0;
  // The separator's own bytes belong to neither neighbour, so a part ends where its
  // separator begins. A stretch that is nothing but separators contributes no index.
  const auto emit = [&](size_t start, size_t end) {
    if (end <= start) return;
    if (n < maxParts) out[n++] = Part{static_cast<uint16_t>(start), static_cast<uint16_t>(end)};
  };

  for (size_t i = 0; i < len;) {
    const size_t d = dashLenAt(text, len, i);
    if (d > 0) {
      i += d;
      continue;
    }
    const size_t stop = partEnd(text, len, i);
    emit(i, stop);
    i = stop;
  }

  return n;
}

int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const std::string& word, EpdFontFamily::Style style,
                       const int8_t tracking) {
  if (word.find(SOFT_HYPHEN_UTF8) == std::string::npos) {
    return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.c_str(), style, tracking));
  }
  std::string sanitized = word;
  size_t pos = 0;
  while ((pos = sanitized.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    sanitized.erase(pos, SOFT_HYPHEN_BYTES);
  }
  return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, sanitized.c_str(), style, tracking));
}

int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const char* text, EpdFontFamily::Style style,
                       const int8_t tracking) {
  if (!text) return 0;
  if (!std::strstr(text, SOFT_HYPHEN_UTF8)) {
    return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, text, style, tracking));  // no copy
  }
  return measureAdvance(renderer, fontId, std::string(text), style, tracking);
}

int16_t measureAdvance(const GfxRenderer& renderer, int fontId, const char* text, size_t len,
                       EpdFontFamily::Style style, const int8_t tracking) {
  return measureAdvance(renderer, fontId, std::string(text, len), style, tracking);
}

}  // namespace PageTokens
