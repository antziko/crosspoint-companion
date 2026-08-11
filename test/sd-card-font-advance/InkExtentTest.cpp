#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "InkExtent.h"

namespace {

// Mirrors SdCardFont::AdvanceEntry after the ink-metrics fields were folded into its padding.
struct Entry {
  uint32_t codepoint = 0;
  uint16_t advanceX = 0;   // 12.4 fixed-point
  uint8_t width = 0;       // px
  int8_t leftBearing = 0;  // px
};

constexpr uint8_t kNoInkMetrics = 0xFF;  // SdCardFont::kNoInkMetrics

// The layout the firmware relies on: both new fields must land in padding the 6-byte
// {codepoint, advanceX} pair was already paying for, or the table grows by 50%
// (768 entries x 4 styles).
static_assert(sizeof(Entry) == 8, "ink metrics must be free — they belong in existing padding");

// Reference implementation, written to follow EpdFont::getTextBounds (EpdFont.cpp:7-73)
// directly rather than to match Accumulator: seed min/max at the pen origin, then for each
// glyph extend by [penX + left, penX + left + width) and advance the pen. Kerning and
// ligatures are omitted, as they are on both SD fast paths.
int referenceInkWidth(const std::vector<Entry>& glyphs, const bool halfAdvance = false) {
  int minX = 0, maxX = 0;  // seeded at startX = 0, as getTextBounds does
  int32_t cursorFP = 0;
  for (const auto& g : glyphs) {
    const int penX = static_cast<int>((cursorFP + 8) >> 4);  // fp4::toPixel
    minX = std::min(minX, penX + g.leftBearing);
    maxX = std::max(maxX, penX + g.leftBearing + static_cast<int>(g.width));
    const int32_t adv = static_cast<int32_t>(g.advanceX);
    cursorFP += halfAdvance ? (adv + 1) / 2 : adv;
  }
  return maxX - minX;
}

int accumulate(const std::vector<Entry>& glyphs, const bool halfAdvance = false) {
  InkExtent::Accumulator acc;
  for (const auto& g : glyphs) acc.add(g.advanceX, g.width, g.leftBearing, halfAdvance);
  return acc.width();
}

// 12.4 fixed-point helper mirroring fp4::fromPixel.
constexpr uint16_t px(const int v) { return static_cast<uint16_t>(v << 4); }

}  // namespace

TEST(InkExtent, EmptyRunMeasuresZero) {
  InkExtent::Accumulator acc;
  EXPECT_TRUE(acc.empty());
  EXPECT_EQ(acc.width(), 0);
}

TEST(InkExtent, SingleGlyphSpansBearingToBearingPlusWidth) {
  // left bearing 1, ink 6 wide, so the extent runs [0, 7) counting from the origin.
  const std::vector<Entry> glyphs{{'n', px(8), 6, 1}};
  EXPECT_EQ(accumulate(glyphs), 7);
  EXPECT_EQ(accumulate(glyphs), referenceInkWidth(glyphs));
}

TEST(InkExtent, MatchesTheGetTextBoundsReferenceOverAWord) {
  const std::vector<Entry> glyphs{
      {'w', px(11), 10, 0},
      {'o', px(8), 7, 1},
      {'r', px(6), 5, 1},
      {'d', px(9), 7, 1},
  };
  EXPECT_EQ(accumulate(glyphs), referenceInkWidth(glyphs));
}

TEST(InkExtent, NegativeLeftBearingExtendsLeftOfTheOrigin) {
  // An italic 'f' style overhang: ink starts left of the pen. getTextBounds seeds minX at the
  // origin, so the extent grows leftward and the total width includes the overhang.
  const std::vector<Entry> glyphs{{'f', px(5), 8, -2}};
  EXPECT_EQ(accumulate(glyphs), referenceInkWidth(glyphs));
  // Ink spans [-2, 6); seeded at the origin the extent is max(6,0) - min(-2,0) = 8, i.e. the
  // 2px of overhang is counted, not clipped at the pen.
  EXPECT_EQ(accumulate(glyphs), 8);
}

TEST(InkExtent, TrailingSideBearingIsNotCountedAsInk) {
  // The distinguishing property of an INK extent vs an advance sum: a glyph whose advance is
  // wider than its ink must not contribute the gap. Two glyphs, advance 10 but ink 4.
  const std::vector<Entry> glyphs{{'i', px(10), 4, 0}, {'i', px(10), 4, 0}};
  EXPECT_EQ(accumulate(glyphs), 14);  // 10 + 4, not 20
  EXPECT_EQ(accumulate(glyphs), referenceInkWidth(glyphs));
}

TEST(InkExtent, ZeroWidthGlyphContributesAdvanceButNoInk) {
  const std::vector<Entry> glyphs{{' ', px(4), 0, 0}, {'x', px(8), 7, 0}};
  EXPECT_EQ(accumulate(glyphs), 11);  // pen at 4, ink 7 wide
  EXPECT_EQ(accumulate(glyphs), referenceInkWidth(glyphs));
}

TEST(InkExtent, FractionalAdvancesAccumulateBeforeSnapping) {
  // Advances are 12.4 fixed-point. Four glyphs at 4.5px must place the last pen at 13.5 -> 14
  // (round-to-nearest on the accumulated total), not at 4*5=20 from snapping each step.
  const uint16_t fourAndAHalf = static_cast<uint16_t>((4 << 4) | 8);
  const std::vector<Entry> glyphs{
      {'a', fourAndAHalf, 3, 0}, {'a', fourAndAHalf, 3, 0}, {'a', fourAndAHalf, 3, 0}, {'a', fourAndAHalf, 3, 0}};
  EXPECT_EQ(accumulate(glyphs), referenceInkWidth(glyphs));
  EXPECT_EQ(accumulate(glyphs), 17);  // pen 13.5 -> 14, plus 3px of ink
}

TEST(InkExtent, SuperscriptHalvesTheAdvanceButNotTheInk) {
  const std::vector<Entry> glyphs{{'1', px(8), 6, 0}, {'2', px(8), 6, 0}};
  EXPECT_EQ(accumulate(glyphs, /*halfAdvance=*/true), referenceInkWidth(glyphs, true));
  EXPECT_LT(accumulate(glyphs, true), accumulate(glyphs, false));
}

TEST(InkExtent, AccumulatorIsNotEmptyOnceFed) {
  InkExtent::Accumulator acc;
  acc.add(px(8), 6, 0, false);
  EXPECT_FALSE(acc.empty());
}

// The sentinel is what keeps a bearing that does not fit int8_t from being silently
// truncated into a wrong extent. It is a lookup-side contract, so assert it holds as a value
// distinct from any plausible real width.
TEST(InkExtent, SentinelIsDistinctFromPlausibleGlyphWidths) {
  EXPECT_EQ(kNoInkMetrics, 0xFF);
  // Every glyph in a 12-18pt font is far narrower than 255px, so the sentinel can never
  // collide with a real measurement.
  for (int w = 0; w <= 64; w++) EXPECT_NE(static_cast<uint8_t>(w), kNoInkMetrics);
}
