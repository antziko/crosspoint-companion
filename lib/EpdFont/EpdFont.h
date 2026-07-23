#pragma once
#include "EpdFontData.h"

class EpdFont {
  void getTextBounds(const char* string, int startX, int startY, int* minX, int* minY, int* maxX, int* maxY) const;

 public:
  const EpdFontData* data;
  explicit EpdFont(const EpdFontData* data) : data(data) {}
  ~EpdFont() = default;
  void getTextDimensions(const char* string, int* w, int* h) const;

  /// Returns the pen advance width (in pixels) of the string: the cursor position
  /// drawText leaves the pen at, using the same fp4 advance + kerning snapping as
  /// getTextBounds. Unlike getTextDimensions (which returns the INK bounding box),
  /// this is suitable for caret/sub-span positioning (e.g. underlining a word).
  int getAdvanceWidth(const char* string) const;

  /// Ink bounding-box horizontal extents relative to a pen origin of 0: minX is the
  /// first glyph's left side bearing, maxX is the rightmost inked pixel. Combine with
  /// getAdvanceWidth(prefix) to place a sub-span (e.g. underline a word at
  /// penOrigin + minX for width maxX - minX).
  void getInkExtents(const char* string, int* minX, int* maxX) const;

  const EpdGlyph* getGlyph(uint32_t cp) const;

  /// Returns true if this font covers `cp`: either via its in-RAM interval
  /// table or, for SD card fonts, via the coverageHandler that consults the
  /// full RAM-resident coverage index. Unlike getGlyph(), it never performs
  /// storage I/O and never falls back to the replacement glyph — it reports
  /// only what this font can render. Used by the CJK UI font fallback to
  /// decide whether a string needs to be routed to another font.
  bool hasCodepoint(uint32_t cp) const;

  /// Returns the kerning adjustment (4.4 fixed-point in pixels) between two codepoints.
  /// Returns 0 if no kerning data exists for the pair.
  int8_t getKerning(uint32_t leftCp, uint32_t rightCp) const;

  /// Returns the ligature codepoint for a pair, or 0 if no ligature exists.
  uint32_t getLigature(uint32_t leftCp, uint32_t rightCp) const;

  /// Greedily applies ligature substitutions starting from cp, consuming
  /// as many following codepoints from text as possible. Returns the
  /// (possibly substituted) codepoint; advances text past consumed chars.
  uint32_t applyLigatures(uint32_t cp, const char*& text) const;
};
