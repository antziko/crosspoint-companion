#include <gtest/gtest.h>

#include <string>

#include "util/Dictionary.h"

// Dictionary::cleanWord trims non-word characters from both edges of a selected
// word before it is looked up. Two classes of bug have lived here:
//
//   1. Using std::isalnum alone rejects every byte >= 0x80, so any word whose
//      edge is non-ASCII lost it ("café" -> "caf") and wholly non-ASCII words
//      collapsed to "" -> the "no word" popup, never a lookup.
//   2. Treating every byte >= 0x80 as a word byte fixes (1) but keeps curly
//      quotes and dashes, so EPUB text like garage.” never matches a headword.
//
// The current implementation must satisfy both at once, and must not touch
// punctuation *inside* a word (don’t, hyphenated forms).

namespace {
std::string clean(const std::string& in) { return Dictionary::cleanWord(in); }
}  // namespace

// --- Baseline ASCII behaviour -------------------------------------------------

TEST(DictCleanWord, TrimsAsciiPunctuationAndLowercases) {
  EXPECT_EQ(clean("hello."), "hello");
  EXPECT_EQ(clean("(Hello)"), "hello");
  EXPECT_EQ(clean("Hello"), "hello");
  EXPECT_EQ(clean("...word!!!"), "word");
}

TEST(DictCleanWord, KeepsInteriorAsciiPunctuation) {
  EXPECT_EQ(clean("don't"), "don't");
  EXPECT_EQ(clean("well-known."), "well-known");
}

// --- Regression 1: non-ASCII edges must survive -------------------------------

TEST(DictCleanWord, KeepsAccentedEdges) {
  EXPECT_EQ(clean("café"), "café");
  EXPECT_EQ(clean("café."), "café");
  EXPECT_EQ(clean("naïve"), "naïve");
  EXPECT_EQ(clean("über"), "über");
}

TEST(DictCleanWord, KeepsWhollyNonAsciiWords) {
  // These previously trimmed to "" and could never be looked up.
  EXPECT_EQ(clean("漢字"), "漢字");
  EXPECT_EQ(clean("日本語の本"), "日本語の本");
  EXPECT_EQ(clean("Привет"), "Привет");
  EXPECT_EQ(clean("(漢字)"), "漢字");
}

// --- Regression 2: General Punctuation must still be trimmed ------------------

TEST(DictCleanWord, TrimsCurlyQuotes) {
  EXPECT_EQ(clean("garage.”"), "garage");
  EXPECT_EQ(clean("“quoted”"), "quoted");
  EXPECT_EQ(clean("‘single’"), "single");
}

TEST(DictCleanWord, TrimsDashesAndEllipsis) {
  EXPECT_EQ(clean("word—"), "word");    // em dash U+2014
  EXPECT_EQ(clean("–word–"), "word");   // en dash U+2013
  EXPECT_EQ(clean("word…"), "word");    // ellipsis U+2026
  EXPECT_EQ(clean("—word.”"), "word");  // mixed classes on both edges
}

TEST(DictCleanWord, KeepsInteriorCurlyApostrophe) {
  // Only edges are trimmed, so a curly apostrophe inside a contraction stays.
  EXPECT_EQ(clean("don’t"), "don’t");
  EXPECT_EQ(clean("“don’t”"), "don’t");
}

// --- Degenerate input ---------------------------------------------------------

TEST(DictCleanWord, EmptyWhenNothingRemains) {
  EXPECT_EQ(clean(""), "");
  EXPECT_EQ(clean("..."), "");
  EXPECT_EQ(clean("“”"), "");
  EXPECT_EQ(clean("—"), "");
  EXPECT_EQ(clean("… — ‘"), "");
}

// --- CJK punctuation ----------------------------------------------------------
//
// Chinese/Japanese punctuation is not in General Punctuation (U+2000-U+206F); it
// lives in CJK Symbols and Punctuation (U+3000-U+303F) and the Fullwidth Forms
// (U+FF01-U+FF65). Both are >= 0x80, so isWordByte kept them and every Chinese
// lookup on a word touching punctuation returned "no result".
//
// These arrive glued to the word rather than as their own token: layout forbids a
// break before closing punctuation and after opening punctuation
// (ParsedText.cpp:150-155), so 國。 and 「中 are each a single token by design.

TEST(DictCleanWord, TrimsTrailingCjkPunctuation) {
  EXPECT_EQ(clean("國。"), "國");      // 。 ideographic full stop U+3002
  EXPECT_EQ(clean("你好，"), "你好");  // ， fullwidth comma U+FF0C
  EXPECT_EQ(clean("走、"), "走");      // 、 ideographic comma U+3001
  EXPECT_EQ(clean("什麼？"), "什麼");  // ？ fullwidth question mark U+FF1F
  EXPECT_EQ(clean("住手！"), "住手");  // ！ fullwidth exclamation U+FF01
}

TEST(DictCleanWord, TrimsLeadingCjkPunctuation) {
  EXPECT_EQ(clean("「中"), "中");  // 「 corner bracket U+300C
  EXPECT_EQ(clean("《紅"), "紅");  // 《 double angle bracket U+300A
  EXPECT_EQ(clean("（漢"), "漢");  // （ fullwidth left paren U+FF08
}

TEST(DictCleanWord, TrimsCjkPunctuationOnBothEdges) {
  EXPECT_EQ(clean("《紅樓夢》"), "紅樓夢");
  EXPECT_EQ(clean("「中國」"), "中國");
  EXPECT_EQ(clean("（漢字）"), "漢字");
  EXPECT_EQ(clean("【注】。"), "注");  // mixed classes stacked on one edge
}

TEST(DictCleanWord, EmptyWhenOnlyCjkPunctuation) {
  EXPECT_EQ(clean("。"), "");
  EXPECT_EQ(clean("。、！"), "");
  EXPECT_EQ(clean("「」"), "");
  EXPECT_EQ(clean("　"), "");  // U+3000 ideographic space
}

// Interior punctuation is left alone, exactly as it is for ASCII (don't) — only
// the edges are trimmed. A multi-select phrase spanning a sentence break keeps it.
TEST(DictCleanWord, KeepsInteriorCjkPunctuation) {
  EXPECT_EQ(clean("中國。他說"), "中國。他說");
  EXPECT_EQ(clean("。中國。他說。"), "中國。他說");
}

// U+3005-U+3007 and U+303B sit inside the CJK Symbols and Punctuation block but
// are content, not punctuation: 々 repeats the preceding character (人々 = "people"),
// 〇 is the ideographic zero. A greedy edge trim over the whole block would reduce
// 人々。 to 人 and 二〇二五 to 二.
TEST(DictCleanWord, KeepsIterationMarksAndIdeographicZero) {
  EXPECT_EQ(clean("人々"), "人々");          // 々 U+3005
  EXPECT_EQ(clean("人々。"), "人々");        // trim the 。 but keep the 々
  EXPECT_EQ(clean("二〇二五"), "二〇二五");  // 〇 U+3007 ideographic zero
  EXPECT_EQ(clean("〆"), "〆");              // U+3006
}

// Fullwidth digits and fullwidth Latin letters are content, not punctuation.
TEST(DictCleanWord, KeepsFullwidthAlphanumerics) {
  EXPECT_EQ(clean("１２３"), "１２３");  // U+FF11-FF13
  EXPECT_EQ(clean("Ａ"), "Ａ");          // U+FF21
  EXPECT_EQ(clean("（Ａ）"), "Ａ");      // trimmed parens, kept the letter
}

TEST(DictCleanWord, HandlesTruncatedUtf8WithoutOverreading) {
  // A lone 0xE2 lead byte has no continuation bytes to inspect; the 3-byte
  // guard must stop the General-Punctuation probe from reading past the end.
  EXPECT_NO_THROW({ (void)clean("\xE2"); });
  EXPECT_NO_THROW({ (void)clean("\xE2\x80"); });
  EXPECT_NO_THROW({ (void)clean("ab\xE2"); });
  // Treated as ordinary word bytes since they cannot form a punctuation
  // codepoint; the point is that they are not dropped and do not crash.
  EXPECT_EQ(clean("\xE2\x80"), "\xE2\x80");
}
