#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Appends a Unicode codepoint to a std::string in UTF-8 encoding.
void utf8AppendCodepoint(uint32_t cp, std::string& out);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Canonical composition (NFC) for the Latin / Vietnamese range: precomposes a
// base letter followed by combining diacritical mark(s) into a single codepoint.
// Needed because the device fonts have no combining-mark positioning, so text
// stored in NFD (e.g. some EPUB chapter titles) otherwise renders broken.
std::string utf8ComposeNfc(const std::string& in);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Returns true for CJK characters that allow line breaks on either side without hyphenation.
// Covers CJK Unified Ideographs, Hiragana, Katakana, Hangul Syllables, CJK punctuation,
// and fullwidth forms — the ranges where word boundaries are implicit per character.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xD7B0 && cp <= 0xD7FF)     // Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Returns true for any codepoint in a CJK script block (Han, Kana, Hangul, Bopomofo,
// radicals, and CJK punctuation/compatibility/enclosed forms). Used for fallback font
// selection — deliberately broader than utf8IsCjkBreakable, whose ranges are tuned to
// implicit line-break opportunities and must not grow without rethinking layout.
inline bool utf8IsCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x2E80 && cp <= 0x2FDF)     // CJK Radicals Supplement, Kangxi Radicals
         || (cp >= 0x3000 && cp <= 0x33FF)     // CJK punctuation, Kana, Bopomofo, Hangul Compat
                                               // Jamo, Kanbun, strokes, enclosed + compat forms
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xA960 && cp <= 0xA97F)     // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)     // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE10 && cp <= 0xFE1F)     // Vertical Forms
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2EBEF)   // CJK Extensions B-F
         || (cp >= 0x2F800 && cp <= 0x2FA1F)   // CJK Compatibility Ideographs Supplement
         || (cp >= 0x30000 && cp <= 0x323AF);  // CJK Extensions G-H
}

// Returns true for CJK punctuation and symbol codepoints — the subset of the CJK blocks
// that carries no lexical content. Both utf8IsCjkBreakable and utf8IsCjkCodepoint include
// these ranges (they are break opportunities and need the CJK fallback font), so callers
// that want CJK *letters* must subtract this set. Deliberately excludes the fullwidth
// digits (U+FF10-FF19) and fullwidth Latin letters (U+FF21-FF3A, U+FF41-FF5A), which are
// content.
inline bool utf8IsCjkPunctuation(const uint32_t cp) {
  return (cp >= 0x3000 && cp <= 0x303F)      // CJK Symbols and Punctuation 。、「」『』【】〔〕
         || (cp >= 0xFE10 && cp <= 0xFE1F)   // Vertical Forms
         || (cp >= 0xFE30 && cp <= 0xFE4F)   // CJK Compatibility Forms (vertical punctuation)
         || (cp >= 0xFF01 && cp <= 0xFF0F)   // Fullwidth ! " # $ % & ' ( ) * + , - . /
         || (cp >= 0xFF1A && cp <= 0xFF20)   // Fullwidth : ; < = > ? @
         || (cp >= 0xFF3B && cp <= 0xFF40)   // Fullwidth [ \ ] ^ _ `
         || (cp >= 0xFF5B && cp <= 0xFF65);  // Fullwidth { | } ~ and halfwidth punctuation
}

// First codepoint of a string, or 0 when empty/null. Takes const char* so callers with a
// raw pool pointer don't materialise a temporary std::string just to read one codepoint.
// Named with the utf8 prefix because ParsedText.cpp has its own file-local
// firstCodepoint/lastCodepoint and includes this header — unprefixed names would make its
// unqualified calls ambiguous.
inline uint32_t utf8FirstCodepoint(const char* s) {
  if (!s || !*s) return 0;
  const auto* ptr = reinterpret_cast<const unsigned char*>(s);
  return utf8NextCodepoint(&ptr);
}

// Last codepoint of a string, or 0 when empty. Scans backward over continuation bytes
// (10xxxxxx) to find the start of the final sequence, then decodes forward from there.
inline uint32_t utf8LastCodepoint(const std::string& s) {
  if (s.empty()) return 0;
  size_t i = s.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(s.c_str()) + i;
  return utf8NextCodepoint(&ptr);
}

// Whether a separating space belongs between text built so far and the next token, when
// re-joining words that layout had already split.
//
// The problem this solves: CJK is written without spaces, and layout tokenises it one word
// per character. Joining those tokens with an unconditional " " turns 中国人民 into
// "中 国 人 民" — which matches no dictionary headword, and which any consumer that splits
// the string back on spaces (the Reader Options preview) then renders as spaced-out text.
// Layout itself records the distinction per word as noSpaceBefore (ParsedText.cpp:351-356);
// this is the rule to apply when that flag is no longer available, and it mirrors layout's
// own hasCjkBreakOpportunityBetween (ParsedText.cpp:150).
//
// Latin text carries no CJK codepoints, so both tests pass and the space is inserted exactly
// as an unconditional join would have.
inline bool utf8NeedsSpaceBetween(const std::string& left, const char* right) {
  if (left.empty() || !right || !*right) return false;
  return !utf8IsCjkBreakable(utf8LastCodepoint(left)) && !utf8IsCjkBreakable(utf8FirstCodepoint(right));
}

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}

// Encode a Unicode codepoint to UTF-8. Writes 1-4 bytes to buf (must be >= 4 bytes).
// Returns the number of bytes written.
int utf8EncodeCodepoint(uint32_t cp, char* buf);

// Append a Unicode codepoint as UTF-8 to a string.
void utf8AppendCodepoint(std::string& str, uint32_t cp);

// As above, but reports failure instead of aborting — for the dictionary wrap path, which
// runs on a few KB of free heap where an unguarded append() reboots the device. On false the
// string is unchanged. See appendNoThrow() in Memory.h for the mechanism and its caveat.
[[nodiscard]] bool utf8AppendCodepointNoThrow(std::string& str, uint32_t cp);

// Returns true if the string ends with '-' or soft-hyphen (U+00AD = 0xC2 0xAD in UTF-8).
inline bool utf8EndsWithHyphen(const char* str, size_t len) {
  if (len == 0) return false;
  if (str[len - 1] == '-') return true;
  return len >= 2 && static_cast<uint8_t>(str[len - 2]) == 0xC2 && static_cast<uint8_t>(str[len - 1]) == 0xAD;
}

// Remove trailing '-' or soft-hyphen (U+00AD) from string.
inline void utf8RemoveTrailingHyphen(std::string& str) {
  if (str.empty()) return;
  if (str.back() == '-') {
    str.pop_back();
  } else if (str.size() >= 2 && static_cast<uint8_t>(str[str.size() - 2]) == 0xC2 &&
             static_cast<uint8_t>(str[str.size() - 1]) == 0xAD) {
    str.erase(str.size() - 2);
  }
}
