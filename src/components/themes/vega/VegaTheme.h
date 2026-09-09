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
// Reserve below each "next 3" thumbnail for its 2-line wrapped title:
// kLineGap + 2 x the label font's line height, which is what VegaTheme.cpp
// actually lays out. SMALL_FONT_ID is notosans_8, whose advanceY is 23
// (builtinFonts/notosans_8_regular.h) -- so 4 + 2*23 = 50, plus 2px slack.
//
// This was 32, from a comment guessing "an 8px face, ~10-14px lines". The face
// is 8 POINT, not 8 pixels; the real line height is 23, so a two-line caption
// overran the tile by 18px. That went unnoticed while the caption was redrawn
// into the framebuffer every frame (the overflow simply painted into the empty
// band above the menu), and became visible as clipped text the moment the
// caption moved into the cover snapshot, which is clipped to exactly this tile
// height. drawNextTitles() now also clamps its line count to the rect, so a
// future font change degrades to fewer lines instead of clipped ones.
//
// Costs no layout: Vega's menu row is bottom-anchored and ignores the rect it
// is passed (see drawButtonMenu), so a taller tile grows into the empty band
// rather than pushing anything down.
constexpr int kNextLabelReserve = 52;
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
  // Both draw calls above lay out on axes the base hit-test cannot express:
  // the hero card sits ABOVE the "next 3" row (two bands, not one strip), and
  // the menu is a horizontal row anchored to the screen bottom. Without these
  // the base grid maps every menu icon to entry 0 and every cover tap to the
  // wrong book.
  bool recentBookIndexFromPoint(const GfxRenderer& renderer, Rect rect, int recentCount, int x, int y,
                                int& index) const override;
  bool menuIndexFromPoint(const GfxRenderer& renderer, Rect rect, int buttonCount, int x, int y,
                          int& index) const override;
};
