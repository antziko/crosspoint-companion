#pragma once

// companion: the part of matcha-reader's ReaderFontScale the vertical engine uses, over our
// CssStyle. Horizontal text sizes its blocks in ChapterHtmlSlimParser::applyBlockFontScale.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "css/CssStyle.h"

// font-size as a multiple of the body (rem, px, pt) or the parent (em, %); 0 when unset.
inline float cssFontSizeScale(const CssStyle& style) {
  if (!style.hasFontSize()) return 0.0f;
  float multiple;
  switch (style.fontSize.unit) {
    case CssUnit::Em:
    case CssUnit::Rem:
      multiple = style.fontSize.value;
      break;
    case CssUnit::Percent:
      multiple = style.fontSize.value / 100.0f;
      break;
    case CssUnit::Points:
      multiple = style.fontSize.value / 12.0f;  // the CSS initial size, 12pt = 16px
      break;
    case CssUnit::Pixels:
    default:
      multiple = style.fontSize.value / 16.0f;
      break;
  }
  if (!(multiple > 0.0f)) return 0.0f;  // also rejects NaN
  return std::clamp(multiple, 0.5f, 4.0f);
}

// True when font-size is relative to the parent (em, %) rather than the body (rem, pt, px).
inline bool cssFontSizeIsParentRelative(const CssStyle& style) {
  return style.hasFontSize() && (style.fontSize.unit == CssUnit::Em || style.fontSize.unit == CssUnit::Percent);
}

// A 256-based scale snapped to eighths, negated (the per-glyph font-scale tag); 0 = the base size.
inline int32_t fontScaleTagForScale(float scale) {
  if (scale <= 0.0f) return 0;
  scale = std::clamp(scale, 0.5f, 4.0f);
  constexpr int SCALE_ONE = 256;
  constexpr int SCALE_STEP = 32;
  int scaled = static_cast<int>(std::lround(scale * SCALE_ONE / SCALE_STEP)) * SCALE_STEP;
  scaled = std::clamp(scaled, 32, 512);
  if (scaled == SCALE_ONE) return 0;
  return -scaled;
}
