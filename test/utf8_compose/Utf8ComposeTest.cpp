#include <gtest/gtest.h>

#include <string>

#include "Utf8.h"

namespace {

// Helpers to build NFD / expected byte sequences explicitly so the test does not
// depend on the encoding of this source file.
const std::string kCombGrave = "\xCC\x80";     // U+0300 COMBINING GRAVE ACCENT
const std::string kCombAcute = "\xCC\x81";     // U+0301 COMBINING ACUTE ACCENT
const std::string kCombCirc = "\xCC\x82";      // U+0302 COMBINING CIRCUMFLEX ACCENT
const std::string kCombDotBelow = "\xCC\xA3";  // U+0323 COMBINING DOT BELOW

}  // namespace

// ASCII and already-precomposed (NFC) text must pass through untouched (fast path).
TEST(Utf8ComposeNfc, PassesThroughAsciiAndNfc) {
  EXPECT_EQ(utf8ComposeNfc(""), "");
  EXPECT_EQ(utf8ComposeNfc("hello world"), "hello world");
  EXPECT_EQ(utf8ComposeNfc("caf\xC3\xA9"), "caf\xC3\xA9");  // é already U+00E9
}

// Single combining mark composes onto its base letter.
TEST(Utf8ComposeNfc, ComposesSingleMark) {
  EXPECT_EQ(utf8ComposeNfc("e" + kCombAcute), "\xC3\xA9");  // e + ́  -> é  (U+00E9)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombGrave), "\xC3\xA0");  // a + ̀  -> à  (U+00E0)
}

// Vietnamese letters carry two stacked marks; composition must accumulate them
// onto the intermediate precomposed character (this is the crux of the feature).
TEST(Utf8ComposeNfc, ComposesStackedVietnameseMarks) {
  // a + circumflex + acute -> ấ (U+1EA5)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombCirc + kCombAcute), "\xE1\xBA\xA5");
  // a + dot-below + circumflex (canonical order) -> ậ (U+1EAD)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombDotBelow + kCombCirc), "\xE1\xBA\xAD");
}

// A combining mark with no composition for its base is left unchanged, and the
// base is preserved.
TEST(Utf8ComposeNfc, LeavesUncomposableMarksIntact) {
  const std::string in = "q" + kCombAcute;  // no precomposed "q with acute"
  EXPECT_EQ(utf8ComposeNfc(in), in);
}

// A leading combining mark (no preceding base) is emitted unchanged.
TEST(Utf8ComposeNfc, HandlesLeadingMark) { EXPECT_EQ(utf8ComposeNfc(kCombAcute), kCombAcute); }

// Marks embedded in a longer word compose while surrounding text is preserved.
TEST(Utf8ComposeNfc, ComposesWithinWord) {
  // "Ti" + e+circ+acute + "ng" -> "Tiếng"
  EXPECT_EQ(utf8ComposeNfc("Ti" + std::string("e") + kCombCirc + kCombAcute + "ng"), "Ti\xE1\xBA\xBFng");
}

// ---------------------------------------------------------------------------
// utf8IsCjkPunctuation — the exclusion set that keeps CJK punctuation from
// becoming a selectable word in DictionaryWordSelectActivity::extractWords.
// ---------------------------------------------------------------------------

TEST(Utf8IsCjkPunctuation, MatchesCjkPunctuation) {
  EXPECT_TRUE(utf8IsCjkPunctuation(0x3002));  // 。 ideographic full stop
  EXPECT_TRUE(utf8IsCjkPunctuation(0x3001));  // 、 ideographic comma
  EXPECT_TRUE(utf8IsCjkPunctuation(0x300C));  // 「 corner bracket
  EXPECT_TRUE(utf8IsCjkPunctuation(0x300D));  // 」
  EXPECT_TRUE(utf8IsCjkPunctuation(0x3010));  // 【
  EXPECT_TRUE(utf8IsCjkPunctuation(0x3000));  // ideographic space
  EXPECT_TRUE(utf8IsCjkPunctuation(0xFF01));  // ！ fullwidth exclamation
  EXPECT_TRUE(utf8IsCjkPunctuation(0xFF0C));  // ， fullwidth comma
  EXPECT_TRUE(utf8IsCjkPunctuation(0xFF1F));  // ？ fullwidth question mark
  EXPECT_TRUE(utf8IsCjkPunctuation(0xFE41));  // vertical corner bracket
}

// The whole point of the predicate: letters and digits must survive it, or the
// characters the user wants to look up get filtered out as punctuation.
TEST(Utf8IsCjkPunctuation, LeavesCjkLettersAndDigitsAlone) {
  EXPECT_FALSE(utf8IsCjkPunctuation(0x4E2D));  // 中
  EXPECT_FALSE(utf8IsCjkPunctuation(0x56FD));  // 国
  EXPECT_FALSE(utf8IsCjkPunctuation(0x304B));  // か hiragana
  EXPECT_FALSE(utf8IsCjkPunctuation(0x30AB));  // カ katakana
  EXPECT_FALSE(utf8IsCjkPunctuation(0xD55C));  // 한 hangul syllable
  EXPECT_FALSE(utf8IsCjkPunctuation(0xFF11));  // １ fullwidth digit one
  EXPECT_FALSE(utf8IsCjkPunctuation(0xFF21));  // Ａ fullwidth capital A
  EXPECT_FALSE(utf8IsCjkPunctuation(0xFF41));  // ａ fullwidth small a
  EXPECT_FALSE(utf8IsCjkPunctuation('a'));     // plain ASCII is not CJK anything
}

// ---------------------------------------------------------------------------
// utf8FirstCodepoint / utf8LastCodepoint
// ---------------------------------------------------------------------------

TEST(Utf8Codepoints, ReadsFirstAndLastOfMultibyteStrings) {
  EXPECT_EQ(utf8FirstCodepoint("\xE4\xB8\xAD\xE5\x9B\xBD"), 0x4E2Du);  // 中国 -> 中
  EXPECT_EQ(utf8LastCodepoint("\xE4\xB8\xAD\xE5\x9B\xBD"), 0x56FDu);   // 中国 -> 国
  EXPECT_EQ(utf8FirstCodepoint("abc"), static_cast<uint32_t>('a'));
  EXPECT_EQ(utf8LastCodepoint("abc"), static_cast<uint32_t>('c'));
  // Mixed: the boundary predicate in buildPhrase depends on these two edges.
  EXPECT_EQ(utf8FirstCodepoint("WiFi\xE5\xAF\x86"), static_cast<uint32_t>('W'));
  EXPECT_EQ(utf8LastCodepoint("WiFi\xE5\xAF\x86"), 0x5BC6u);  // 密
}

TEST(Utf8Codepoints, HandlesEmptyAndNull) {
  EXPECT_EQ(utf8FirstCodepoint(""), 0u);
  EXPECT_EQ(utf8FirstCodepoint(nullptr), 0u);
  EXPECT_EQ(utf8LastCodepoint(std::string()), 0u);
}

// A single multibyte character: the backward scan must walk over its continuation
// bytes and land on the lead byte, not return a continuation byte's value.
TEST(Utf8Codepoints, LastCodepointOfSingleMultibyteChar) {
  EXPECT_EQ(utf8LastCodepoint("\xE4\xB8\xAD"), 0x4E2Du);       // 中
  EXPECT_EQ(utf8LastCodepoint("\xF0\xA0\x80\x8B"), 0x2000Bu);  // 4-byte CJK ext B
}

// utf8NeedsSpaceBetween is the shared "should a space go here" rule used wherever words that
// layout already split have to be re-joined into a plain string: WordSelectNavigator's phrase
// lookup and Section's page-text flattening (which feeds the Reader Options preview and the
// QR text). Both lost the noSpaceBefore flag layout recorded, so they reconstruct it here.
TEST(Utf8NeedsSpaceBetween, KeepsSpacesBetweenLatinWords) {
  EXPECT_TRUE(utf8NeedsSpaceBetween("hello", "world"));
  EXPECT_TRUE(utf8NeedsSpaceBetween("the quick brown", "fox"));
  // Punctuation is not CJK, so it is spaced exactly as an unconditional join would.
  EXPECT_TRUE(utf8NeedsSpaceBetween("end.", "Next"));
}

TEST(Utf8NeedsSpaceBetween, SuppressesSpacesInsideCjkRuns) {
  EXPECT_FALSE(utf8NeedsSpaceBetween("\xE4\xB8\xAD", "\xE5\x9B\xBD"));  // 中 + 国
  // Kana and fullwidth forms are covered by the same predicate as Han.
  EXPECT_FALSE(utf8NeedsSpaceBetween("\xE3\x81\x8B", "\xE3\x81\xAA"));  // か + な
  EXPECT_FALSE(utf8NeedsSpaceBetween("\xEF\xBC\x91", "\xEF\xBC\x92"));  // fullwidth 1 + 2
}

// Either side being CJK is enough. This is the boundary a naive "both sides" test gets
// wrong, and it is what keeps 中国 from being split off an adjacent Latin word.
TEST(Utf8NeedsSpaceBetween, SuppressesAtMixedScriptBoundaries) {
  EXPECT_FALSE(utf8NeedsSpaceBetween("WiFi", "\xE5\xAF\x86"));  // WiFi + 密
  EXPECT_FALSE(utf8NeedsSpaceBetween("\xE5\xAF\x86", "WiFi"));  // 密 + WiFi
}

// Nothing to separate from: the first token of a string never gets a leading space, and an
// empty token never provokes one.
TEST(Utf8NeedsSpaceBetween, NoSpaceAtTheEdges) {
  EXPECT_FALSE(utf8NeedsSpaceBetween("", "word"));
  EXPECT_FALSE(utf8NeedsSpaceBetween("word", ""));
  EXPECT_FALSE(utf8NeedsSpaceBetween("word", nullptr));
  EXPECT_FALSE(utf8NeedsSpaceBetween("", ""));
}
