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
