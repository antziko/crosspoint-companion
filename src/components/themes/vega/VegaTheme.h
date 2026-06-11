#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Vega — "spotlight on one star": one hero book (cover + progress + chapter +
// last-read) front and center, a compact "next 3" row below, and a horizontal
// icon-only menu anchored to the screen bottom. Sibling of LyraTheme, same
// derivation convention as Lyra3CoversTheme (LyraTheme.h:18-style inheritance).
namespace VegaMetrics {
// Hero-card layout constants, shared with VegaTheme.cpp's drawing code so the
// tile-height computation below and the actual draw geometry can't drift apart.
constexpr int kHeroPadding = 20;
constexpr int kSectionGap = 20;
// Reserve below each "next 3" thumbnail for its 2-line wrapped title
// (label-font line-height x2 + line-gap, computed at runtime in
// VegaTheme.cpp -- this is a fixed upper-bound margin for the constexpr
// tile-height calc below; SMALL_FONT_ID is an 8px face, ~10-14px lines).
constexpr int kNextLabelReserve = 32;
// The hero cover draws taller than the "next 3" row. The cached thumbnail is
// generated once at the hero height (homeCoverHeight is the cache key/size,
// see HomeActivity::loadRecentCovers); the hero draws it at native size and
// the row tiles vertically crop it down to kNextRowCoverHeight. Cropping
// (not scaling) keeps GfxRenderer::drawBitmap's fitScale at 1.0 everywhere --
// any nearest-neighbour downscale visibly darkens the pre-dithered 1-bit
// cover bitmaps (collapsed source pixels OR-composite toward black: a
// 2-into-1 collapse of a ~50%-dithered region renders ~75% black).
constexpr int kHeroExtraHeight = 12;
constexpr int kNextRowCoverHeight = LyraMetrics::values.homeCoverHeight;

constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  // homeCoverHeight doubles as the hero draw height and the thumbnail
  // generation height; the row crops ~5% off (top+bottom slivers).
  v.homeCoverHeight = kNextRowCoverHeight + kHeroExtraHeight;
  // Tile = hero cover + padding + "next 3" row (cropped cover height +
  // title reserve) beneath it, vs. Lyra's single cover-and-title tile.
  const int nextRowHeight = kNextRowCoverHeight + kNextLabelReserve;
  v.homeCoverTileHeight = v.homeCoverHeight + 2 * kHeroPadding + kSectionGap + nextRowHeight;
  v.homeRecentBooksCount = 4;
  return v;
}();
}  // namespace VegaMetrics

class VegaTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
};
