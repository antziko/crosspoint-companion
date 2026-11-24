#pragma once
#include <Memory.h>
#include <Utf8.h>

#include <cstdint>
#include <string>
#include <vector>

/// Returns true if the Unicode codepoint falls within an IPA phonetic range.
/// Ranges covered:
///   U+0250–U+02AF  IPA Extensions
///   U+02B0–U+02FF  Modifier Letters (IPA subset)
///   U+1D00–U+1D7F  Phonetic Extensions
///   U+1D80–U+1DBF  Phonetic Extensions Supplement
/// Additional IPA characters outside those blocks:
///   U+00E6, U+00F0, U+00F8, U+0127, U+014B, U+0153, U+03B2, U+03B8, U+03C7
static inline bool isIpaCodepoint(uint32_t cp) {
  return cp == 0x00E6 || cp == 0x00F0 || cp == 0x00F8 || cp == 0x0127 || cp == 0x014B || cp == 0x0153 || cp == 0x03B2 ||
         cp == 0x03B8 || cp == 0x03C7 || (cp >= 0x0250 && cp <= 0x02FF) || (cp >= 0x1D00 && cp <= 0x1DBF);
}

struct IpaTextSpan {
  std::string text;
  bool isIpa;
};

/// A codepoint that may need the IPA font: an IPA symbol, or a combining mark.
static inline bool isIpaCandidate(uint32_t cp) { return isIpaCodepoint(cp) || utf8IsCombiningMark(cp); }

/// Which of the two fonts can draw a codepoint. fn(ctx, ipaFont, cp) answers for the IPA font
/// when ipaFont is true and for the body font otherwise. Without fn, the built-in assumption:
/// the body font lacks IPA symbols and combining marks, and the IPA font has those plus ASCII.
struct IpaFontCoverage {
  void* ctx = nullptr;
  bool (*fn)(void* ctx, bool ipaFont, uint32_t cp) = nullptr;

  bool has(bool ipaFont, uint32_t cp) const {
    if (fn) return fn(ctx, ipaFont, cp);
    if (ipaFont) return (cp >= 0x20 && cp <= 0x7E) || isIpaCandidate(cp);
    return !isIpaCandidate(cp);
  }
  /// True when cp sends its word to the IPA font: the body font cannot draw it and the IPA font can.
  bool needsIpa(uint32_t cp) const { return isIpaCandidate(cp) && !has(false, cp) && has(true, cp); }
};

/// True if the word [begin, end) is drawn with the IPA font. A whole word moves, not single
/// characters, so the letters and symbols of a pronunciation share one size and style, and a
/// combining mark always sits on a base letter from its own font (t + U+032C in [ˈkuː.t̬ɚ]).
static inline bool wordNeedsIpaFont(const char* begin, const char* end, const IpaFontCoverage& cov) {
  const auto* p = reinterpret_cast<const uint8_t*>(begin);
  const auto* e = reinterpret_cast<const uint8_t*>(end);
  while (p < e) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (cov.needsIpa(cp)) return true;
  }
  return false;
}

/// True if any word of `text` is drawn with the IPA font, i.e. whether splitIpaRuns() would
/// produce more than the single non-IPA run covering the whole string.
///
/// Allocation-free by design: it is the guard that lets the wrapper skip splitIpaRuns()
/// entirely for the common case. A dictionary definition in CJK — or any Latin text without
/// phonetics — needs no IPA font, and splitting it copies the whole string onto the heap only
/// to hand it straight back.
static inline bool textHasIpa(const char* text, const IpaFontCoverage& cov = {}) {
  if (!text || !text[0]) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    if (cov.needsIpa(cp)) return true;
  }
  return false;
}

/// Split a UTF-8 string into runs for the body font and the IPA font, word by word (words are
/// separated by ASCII spaces, which join whichever run they follow). Inside an IPA word a
/// character the IPA font lacks stays in the body font; a combining mark follows its base.
/// forceWordIpa (0 or 1) classifies every word that way instead of testing it, for a piece of
/// a word that was already classified whole (DictLayout's breakToken).
/// Results are appended into `out`; caller must clear `out` before each call.
///
/// Returns false if an allocation failed, in which case `out` holds whatever runs completed
/// and the caller must treat the split as unusable (a partial split drops text). Every growth
/// here is guarded because this runs inside the dictionary wrap, which the device logs show
/// executing at ~2 KB of free heap — an unguarded append() or push_back() aborts under
/// -fno-exceptions and reboots the device mid-lookup.
[[nodiscard]] static inline bool splitIpaRuns(const char* text, std::vector<IpaTextSpan>& out,
                                              const IpaFontCoverage& cov = {}, int forceWordIpa = -1) {
  if (!text || !text[0]) return true;
  std::string current;
  bool currentIsIpa = false;
  const auto pushRun = [&]() {
    if (out.size() == out.capacity() && !reserveNoThrow(out, out.capacity() ? out.capacity() * 2 : 4)) return false;
    out.push_back({std::move(current), currentIsIpa});
    current.clear();
    return true;
  };
  const char* p = text;
  while (*p) {
    if (*p == ' ') {
      if (!appendNoThrow(current, p, 1)) return false;
      ++p;
      continue;
    }
    const char* wordEnd = p;
    while (*wordEnd && *wordEnd != ' ') ++wordEnd;
    const bool wordIsIpa = forceWordIpa >= 0 ? forceWordIpa != 0 : wordNeedsIpaFont(p, wordEnd, cov);
    const auto* cpPtr = reinterpret_cast<const uint8_t*>(p);
    const auto* cpEnd = reinterpret_cast<const uint8_t*>(wordEnd);
    bool prevIsIpa = false;
    bool wordStart = true;
    while (cpPtr < cpEnd) {
      const uint32_t cp = utf8NextCodepoint(&cpPtr);
      if (cp == 0) break;
      // A mark opening a word (Cambridge writes "t ̬ɚ": the caron after a space) takes the word's
      // font, not the previous word's.
      const bool ipa = utf8IsCombiningMark(cp) && !wordStart ? prevIsIpa : wordIsIpa && cov.has(true, cp);
      prevIsIpa = ipa;
      wordStart = false;
      if (!current.empty() && ipa != currentIsIpa && !pushRun()) return false;
      currentIsIpa = ipa;
      if (!utf8AppendCodepointNoThrow(current, cp)) return false;
    }
    p = wordEnd;
  }
  if (!current.empty() && !pushRun()) return false;
  return true;
}
