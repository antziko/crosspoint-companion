// Host tests for CodepointFreq — the packing that lets SdCardFont::prewarmStyle rank glyphs
// by page frequency when it cannot afford all of them.
//
// The risk this guards is narrow and severe: the count shares a word with the codepoint, so
// any overflow of the count silently corrupts the codepoint and the font renders the wrong
// glyph. Saturation and full-range round-tripping are therefore the point of this suite.

#include <gtest/gtest.h>

#include "CodepointFreq.h"

namespace cf = CodepointFreq;

// A freshly seen codepoint round-trips with count 1.
TEST(CodepointFreq, PackRoundTrips) {
  for (const uint32_t cp : {0u, 0x41u, 0x4E2Du, 0xFFFDu, 0xFFFFu, 0x10000u, 0x10FFFFu}) {
    const uint32_t packed = cf::pack(cp);
    EXPECT_EQ(cf::value(packed), cp) << "codepoint U+" << std::hex << cp;
    EXPECT_EQ(cf::freq(packed), 1u) << "codepoint U+" << std::hex << cp;
  }
}

// The whole Unicode range fits in the value half. U+10FFFF is the boundary that would start
// eating count bits if VALUE_BITS were ever reduced.
TEST(CodepointFreq, MaxCodepointDoesNotCollideWithCount) {
  const uint32_t packed = cf::packWith(cf::MAX_CODEPOINT, 7);
  EXPECT_EQ(cf::value(packed), cf::MAX_CODEPOINT);
  EXPECT_EQ(cf::freq(packed), 7u);
}

// Repeated sightings accumulate, and the codepoint is untouched throughout.
TEST(CodepointFreq, BumpAccumulatesWithoutDisturbingTheCodepoint) {
  uint32_t packed = cf::pack(0x4E2D);  // 中
  for (uint32_t expected = 2; expected <= 50; expected++) {
    packed = cf::bump(packed);
    EXPECT_EQ(cf::freq(packed), expected);
    EXPECT_EQ(cf::value(packed), 0x4E2Du);
  }
}

// The failure that would be silent and disastrous: a count that wraps into the codepoint
// bits. A page cannot really contain 2047 of one character, but the buffer is fed arbitrary
// book text and must not corrupt a glyph if it ever does.
TEST(CodepointFreq, BumpSaturatesInsteadOfCorruptingTheCodepoint) {
  uint32_t packed = cf::packWith(0x4E2D, cf::FREQ_MAX);
  EXPECT_EQ(cf::freq(packed), cf::FREQ_MAX);
  for (int i = 0; i < 1000; i++) {
    packed = cf::bump(packed);
    ASSERT_EQ(cf::freq(packed), cf::FREQ_MAX) << "count must clamp, not wrap";
    ASSERT_EQ(cf::value(packed), 0x4E2Du) << "codepoint must survive a saturated count";
  }
}

// packWith clamps rather than letting an over-large count spill upward.
TEST(CodepointFreq, PackWithClampsExcessiveCounts) {
  const uint32_t packed = cf::packWith(0x4E2D, cf::FREQ_MAX + 500);
  EXPECT_EQ(cf::freq(packed), cf::FREQ_MAX);
  EXPECT_EQ(cf::value(packed), 0x4E2Du);
}

// prewarm() sorts its buffer by value() so the interval builder still sees ascending
// codepoints; a naive sort on the raw packed word would order by count instead. This pins
// the property that makes that comparator correct: ordering by value() is independent of the
// counts riding along.
TEST(CodepointFreq, ValueOrderingIsIndependentOfCount) {
  const uint32_t low = cf::packWith(0x4E00, cf::FREQ_MAX);  // rare codepoint, huge count
  const uint32_t high = cf::packWith(0x9FFF, 1);            // later codepoint, tiny count
  EXPECT_LT(cf::value(low), cf::value(high));
  EXPECT_GT(low, high) << "raw comparison is count-dominated - hence the custom comparator";
}
