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
/// Combining marks used in IPA are attached to the previous run by splitIpaRuns().
static inline bool isIpaCodepoint(uint32_t cp) {
  return cp == 0x00E6 || cp == 0x00F0 || cp == 0x00F8 || cp == 0x0127 || cp == 0x014B || cp == 0x0153 || cp == 0x03B2 ||
         cp == 0x03B8 || cp == 0x03C7 || (cp >= 0x0250 && cp <= 0x02FF) || (cp >= 0x1D00 && cp <= 0x1DBF);
}

struct IpaTextSpan {
  std::string text;
  bool isIpa;
};

/// True if `text` contains at least one IPA codepoint, i.e. whether splitIpaRuns() would
/// produce more than the single non-IPA run covering the whole string.
///
/// Allocation-free by design: it is the guard that lets the wrapper skip splitIpaRuns()
/// entirely for the common case. A dictionary definition in CJK — or any Latin text without
/// phonetics — carries zero IPA codepoints, and splitting it copies the whole string onto the
/// heap only to hand it straight back. Combining marks are not tested here: they inherit the
/// previous run's classification in splitIpaRuns(), so a leading combining mark on non-IPA text
/// still yields one non-IPA run.
static inline bool textHasIpa(const char* text) {
  if (!text || !text[0]) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    if (!utf8IsCombiningMark(cp) && isIpaCodepoint(cp)) return true;
  }
  return false;
}

/// Split a UTF-8 string into runs of IPA vs non-IPA codepoints.
/// Results are appended into `out`; caller must clear `out` before each call.
///
/// Returns false if an allocation failed, in which case `out` holds whatever runs completed
/// and the caller must treat the split as unusable (a partial split drops text). Every growth
/// here is guarded because this runs inside the dictionary wrap, which the device logs show
/// executing at ~2 KB of free heap — an unguarded append() or push_back() aborts under
/// -fno-exceptions and reboots the device mid-lookup.
[[nodiscard]] static inline bool splitIpaRuns(const char* text, std::vector<IpaTextSpan>& out) {
  if (!text || !text[0]) return true;
  std::string current;
  bool currentIsIpa = false;
  bool first = true;
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p))) {
    const bool combining = utf8IsCombiningMark(cp);
    const bool ipa = combining ? currentIsIpa : isIpaCodepoint(cp);
    if (!first && !combining && ipa != currentIsIpa) {
      if (out.size() == out.capacity() && !reserveNoThrow(out, out.capacity() ? out.capacity() * 2 : 4)) return false;
      out.push_back({std::move(current), currentIsIpa});
      current.clear();
    }
    currentIsIpa = ipa;
    first = false;
    if (!utf8AppendCodepointNoThrow(current, cp)) return false;
  }
  if (!current.empty()) {
    if (out.size() == out.capacity() && !reserveNoThrow(out, out.capacity() ? out.capacity() * 2 : 4)) return false;
    out.push_back({std::move(current), currentIsIpa});
  }
  return true;
}
