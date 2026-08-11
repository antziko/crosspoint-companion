#pragma once

#include <algorithm>
#include <cstdint>

#include "EpdFontData.h"  // fp4

// Ink-extent accumulation for text measured from an SD font's metrics table rather than from
// loaded glyphs. Pure and allocation-free so it can be host-tested (see
// test/sd-card-font-advance) and so the arithmetic lives in one place instead of being
// duplicated between GfxRenderer::getSdInkWidth and EpdFont::getTextBounds.
//
// Mirrors what EpdFont::getTextBounds computes (EpdFont.cpp:7-73), minus kerning and
// ligatures — neither is resident during layout, matching the getTextAdvanceX fast path.
// Combining marks are NOT handled here: they anchor over the preceding base glyph via
// combiningMark::anchorOver, which needs that glyph's real metrics, so callers bail to the
// glyph path instead of feeding them in.
namespace InkExtent {

class Accumulator {
 public:
  // Feed one base glyph, in visual order. `advanceFP` is 12.4 fixed-point; `width` and
  // `leftBearing` are pixels. `halfAdvance` reproduces the superscript/subscript halving the
  // advance fast path applies.
  void add(const uint16_t advanceFP, const uint8_t width, const int8_t leftBearing, const bool halfAdvance) {
    const int penX = fp4::toPixel(cursorFP_);
    const int inkLeft = penX + leftBearing;
    const int inkRight = inkLeft + static_cast<int>(width);
    if (!any_) {
      minX_ = inkLeft;
      maxX_ = inkRight;
      any_ = true;
    } else {
      minX_ = std::min(minX_, inkLeft);
      maxX_ = std::max(maxX_, inkRight);
    }
    const int32_t adv = static_cast<int32_t>(advanceFP);
    cursorFP_ += halfAdvance ? (adv + 1) / 2 : adv;
  }

  // getTextBounds seeds min/max at the pen origin (EpdFont.cpp:9-12) before inspecting any
  // glyph, so the extent always spans x=0 and an empty run measures 0.
  int width() const {
    if (!any_) return 0;
    return std::max(maxX_, 0) - std::min(minX_, 0);
  }

  bool empty() const { return !any_; }

 private:
  int32_t cursorFP_ = 0;  // 12.4 fixed-point pen position
  int minX_ = 0;
  int maxX_ = 0;
  bool any_ = false;
};

}  // namespace InkExtent
