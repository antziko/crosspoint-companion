#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "../../../../src/fontIds.h"

// The built-in reader family's sizes, for lines a book sizes apart from the reader font
// (headings, small print). Lines store a point size, not a font id, so a cached section stays
// valid whatever ids the font build assigns.
namespace BuiltinFontSizes {

inline constexpr uint8_t POINTS[] = {12, 14, 16, 18};
inline constexpr int FONT_IDS[] = {NOTOSANS_12_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_16_FONT_ID,
                                   NOTOSANS_18_FONT_ID};
static_assert(std::size(POINTS) == std::size(FONT_IDS), "one id per built-in size");

// The built-in font at `pt`, or 0 when there is none.
inline int fontIdForPt(const uint8_t pt) {
  for (size_t i = 0; i < std::size(POINTS); ++i) {
    if (POINTS[i] == pt) return FONT_IDS[i];
  }
  return 0;
}

// Point size to lay a block out at when it is `scale` times the reader font `fontId`, or 0 to keep
// the reader font (not a built-in, or no size differs). Shrinking rounds down; growing takes the
// nearest size.
inline uint8_t variantPt(const int fontId, const float scale) {
  if (std::fabs(scale - 1.0f) < 0.05f) return 0;
  size_t base = std::size(FONT_IDS);
  for (size_t i = 0; i < std::size(FONT_IDS); ++i) {
    if (FONT_IDS[i] == fontId) base = i;
  }
  if (base == std::size(FONT_IDS)) return 0;
  const float target = static_cast<float>(POINTS[base]) * scale;
  size_t pick = scale < 1.0f ? 0 : base;
  for (size_t i = 0; i < std::size(POINTS); ++i) {
    const float points = POINTS[i];
    if (scale < 1.0f ? points <= target : std::fabs(points - target) < std::fabs(POINTS[pick] - target)) pick = i;
  }
  return pick == base ? 0 : POINTS[pick];
}

}  // namespace BuiltinFontSizes
