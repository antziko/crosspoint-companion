#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Line {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
};

// Fixture metrics (stub renderer): every glyph is 8 px wide, a space is 4 px, kerning is zero.
// Left-aligned so the assertions read natural advances rather than justification stretch.
std::vector<Line> layout(const std::vector<const char*>& words, const uint16_t width, const int8_t characterSpacing,
                         const uint8_t wordSpacingPercent) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  for (const char* word : words) text.addWord(word, EpdFontFamily::REGULAR);
  std::vector<Line> lines;
  text.layoutAndExtractLines(
      renderer, 0, width,
      [&](std::shared_ptr<TextBlock> block, auto) {
        auto& line = lines.emplace_back();
        for (uint16_t i = 0; i < block->wordCount(); ++i) {
          line.words.emplace_back(block->wordText(i));
          line.xpos.push_back(block->wordXpos(i));
        }
      },
      true, characterSpacing, wordSpacingPercent);
  return lines;
}

}  // namespace

// Han splits into one token per character (Hangul does not -- see SECTION_FILE_VERSION v57,
// which gave Korean CSS word-break: keep-all), so the gaps between Han tokens are exactly the
// tracking while the gap across the real space is the scaled space advance. One layout
// therefore exercises both controls and shows they do not contaminate each other.
TEST(TextSpacing, TrackingSeparatesCjkTokensAndScalesWordSpaces) {
  const auto lines = layout({"漢字語", "文書"}, 200, -1, 150);
  ASSERT_EQ(lines.size(), 1u);
  const auto& xpos = lines[0].xpos;
  ASSERT_EQ(xpos.size(), 5u);
  EXPECT_EQ(xpos[0], 0);
  EXPECT_EQ(xpos[1], 7);   // 8 px glyph, -1 px tracking
  EXPECT_EQ(xpos[2], 14);  // same again
  EXPECT_EQ(xpos[3], 28);  // 8 px glyph plus 150% of a 4 px space, no tracking across a space
  EXPECT_EQ(xpos[4], 35);  // 8 px glyph, -1 px tracking
}

// Positive tracking must widen the same line rather than only shifting it.
TEST(TextSpacing, PositiveTrackingWidensTheLine) {
  const auto tight = layout({"漢字語"}, 200, 0, 100);
  const auto loose = layout({"漢字語"}, 200, 2, 100);
  ASSERT_EQ(tight.size(), 1u);
  ASSERT_EQ(loose.size(), 1u);
  ASSERT_EQ(tight[0].xpos.size(), 3u);
  EXPECT_EQ(tight[0].xpos[2], 16);
  EXPECT_EQ(loose[0].xpos[2], 20);  // two 2 px gaps accumulated
}

// Tracking inside a single Latin token has to reach the width the breaker measures, not
// only the draw positions -- otherwise a token draws wider than layout reserved for it and
// overruns the right margin.
TEST(TextSpacing, TrackingWidensASingleLatinToken) {
  // "abcd" is 4 glyphs = 32 px plus 3 inter-glyph gaps: 32 px untracked, 38 px at +2.
  // A 34 px viewport therefore holds it in one line only while tracking is off.
  EXPECT_EQ(layout({"abcd"}, 34, 0, 100).size(), 1u);
  EXPECT_GT(layout({"abcd"}, 34, 2, 100).size(), 1u);
}

// Word spacing has to feed the break calculation, not just the draw positions: two 16 px
// words plus a 4 px space fit in 36 px, and stretching the space past 100% must wrap.
TEST(TextSpacing, WordSpacingChangesWrapThreshold) {
  for (const uint8_t percent : {50, 100, 125, 200}) {
    const auto lines = layout({"ab", "cd"}, 36, 0, percent);
    EXPECT_EQ(lines.size(), percent > 100 ? 2u : 1u) << "at " << static_cast<int>(percent) << '%';
  }
}

// A cached line is drawn straight from the section file, so the tracking it was laid out
// with has to survive the round trip or the glyphs land off their measured positions.
TEST(TextSpacing, SerializedBlockRestoresCharacterSpacing) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText text(false, false, false, style);
  text.addWord("漢字語", EpdFontFamily::REGULAR);
  const auto path = (std::filesystem::temp_directory_path() / "crosspoint-text-spacing.bin").string();

  std::shared_ptr<TextBlock> original;
  text.layoutAndExtractLines(
      renderer, 0, 200, [&](std::shared_ptr<TextBlock> block, auto) { original = std::move(block); }, true, -2, 100);
  ASSERT_NE(original, nullptr);
  ASSERT_EQ(original->getBlockStyle().characterSpacing, -2);

  {
    HalFile out;
    ASSERT_TRUE(out.open(path.c_str(), "wb"));
    ASSERT_TRUE(original->serialize(out));
  }
  HalFile in;
  ASSERT_TRUE(in.open(path.c_str(), "rb"));
  const auto cached = TextBlock::deserialize(in);
  ASSERT_NE(cached, nullptr);
  EXPECT_EQ(cached->getBlockStyle().characterSpacing, -2);
  ASSERT_EQ(cached->wordCount(), original->wordCount());
  for (uint16_t i = 0; i < original->wordCount(); ++i) EXPECT_EQ(cached->wordXpos(i), original->wordXpos(i));
  EXPECT_EQ(in.position(), in.size());  // the reader consumed exactly what the writer produced
  in.close();
  std::filesystem::remove(path);
}
