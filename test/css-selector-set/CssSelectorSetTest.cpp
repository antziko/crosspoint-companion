#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "CssParser.h"

namespace {

std::string tempPath(const char* name) { return ::testing::TempDir() + name; }

void writeFile(const std::string& path, const std::string& content) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fwrite(content.data(), 1, content.size(), f);
  std::fclose(f);
}

bool scanText(CssParser::SelectorSet& set, const std::string& xhtml) {
  const std::string path = tempPath("selector_set.xhtml");
  writeFile(path, xhtml);
  HalFile file;
  if (!file.open(path.c_str(), "rb")) return false;
  return set.scan(file);
}

TEST(CssSelectorSet, CollectsTagsAndClassTokens) {
  CssParser::SelectorSet set;
  ASSERT_TRUE(scanText(set, "<html><body><p class=\"a b\">x</p><div class=\"c\"/></body></html>"));
  EXPECT_TRUE(set.wants("p"));
  EXPECT_TRUE(set.wants("body"));
  EXPECT_TRUE(set.wants(".a"));
  EXPECT_TRUE(set.wants(".b"));
  EXPECT_TRUE(set.wants("p.a"));
  EXPECT_TRUE(set.wants("div.c"));
  EXPECT_FALSE(set.wants("span"));
  EXPECT_FALSE(set.wants(".zz"));
  EXPECT_FALSE(set.wants("h1.a"));  // class used, tag not
  EXPECT_FALSE(set.wants("p.zz"));  // tag used, class not
}

TEST(CssSelectorSet, ClassValueWhitespaceAndQuotes) {
  CssParser::SelectorSet set;
  ASSERT_TRUE(scanText(set, "<p class='  one\ttwo\n three  '>x</p><span class = \"four\">y</span>"));
  EXPECT_TRUE(set.wants(".one"));
  EXPECT_TRUE(set.wants(".two"));
  EXPECT_TRUE(set.wants(".three"));
  EXPECT_TRUE(set.wants(".four"));
  EXPECT_FALSE(set.wants(".one two"));
}

TEST(CssSelectorSet, OtherAttributesDoNotLeak) {
  CssParser::SelectorSet set;
  ASSERT_TRUE(scanText(set,
                       "<a href=\"x>y\" title='class=\"fake\"' classname=\"nope\" data-class=\"no\" "
                       "class=\"real\">t</a>"));
  EXPECT_TRUE(set.wants("a.real"));
  EXPECT_FALSE(set.wants(".fake"));
  EXPECT_FALSE(set.wants(".nope"));
  EXPECT_FALSE(set.wants(".no"));
  EXPECT_FALSE(set.wants("y"));  // '>' inside a quoted value does not end the tag
}

TEST(CssSelectorSet, SkipsDeclarationsAndEndTags) {
  CssParser::SelectorSet set;
  ASSERT_TRUE(scanText(set,
                       "<?xml version=\"1.0\"?><!DOCTYPE html><html><!-- note --><body>"
                       "<br/><img class=\"pic\" src=\"a.png\"/></body></html>"));
  EXPECT_TRUE(set.wants("br"));
  EXPECT_TRUE(set.wants("img.pic"));
  EXPECT_FALSE(set.wants("xml"));
  EXPECT_FALSE(set.wants("doctype"));
}

TEST(CssSelectorSet, CaseInsensitiveLikeNormalizedSelectors) {
  CssParser::SelectorSet set;
  ASSERT_TRUE(scanText(set, "<P class=\"Title\">x</P>"));
  EXPECT_TRUE(set.wants("p"));
  EXPECT_TRUE(set.wants("p.title"));
}

TEST(CssSelectorSet, TokenAcrossReadBufferBoundary) {
  // The scan reads 1KB at a time; place a class token across that boundary.
  std::string xhtml = "<html><body><p>" + std::string(1024 - 33, 'x') + "</p><p class=\"straddle\">y</p></body></html>";
  CssParser::SelectorSet set;
  ASSERT_TRUE(scanText(set, xhtml));
  EXPECT_TRUE(set.wants("p.straddle"));
}

TEST(CssSelectorSet, TooManyNamesFallsBackToEveryRule) {
  std::string xhtml = "<body>";
  for (int i = 0; i < CssParser::SelectorSet::CAPACITY + 10; ++i) {
    xhtml += "<p class=\"c" + std::to_string(i) + "\"/>";
  }
  xhtml += "</body>";
  CssParser::SelectorSet set;
  EXPECT_FALSE(scanText(set, xhtml));
  EXPECT_TRUE(set.wants(".anything"));
}

TEST(CssSelectorSet, UnscannedSetWantsEverything) {
  const CssParser::SelectorSet set;
  EXPECT_TRUE(set.wants("p"));
  EXPECT_TRUE(set.wants(".x"));
}

TEST(CssSelectorSet, LoadFromCacheKeepsOnlyChapterRules) {
  const std::string dir = ::testing::TempDir();
  const std::string cssPath = tempPath("selector_set.css");
  writeFile(cssPath,
            "p { text-align: right; }\n"
            ".used { font-style: italic; }\n"
            ".unused { font-weight: bold; }\n"
            "h1.used { text-decoration: underline; }\n"
            "span.used { text-align: center; }\n");
  CssParser parser{dir};
  {
    HalFile css;
    ASSERT_TRUE(css.open(cssPath.c_str(), "rb"));
    ASSERT_TRUE(parser.loadFromStream(css));
  }
  ASSERT_EQ(parser.ruleCount(), 5u);
  ASSERT_TRUE(parser.saveToCache());

  CssParser::SelectorSet set;
  ASSERT_TRUE(scanText(set, "<body><p class=\"used\">x</p><span>y</span></body>"));
  ASSERT_TRUE(parser.loadFromCache(&set));
  EXPECT_EQ(parser.ruleCount(), 3u);  // p, .used, span.used

  const CssStyle style = parser.resolveStyle("p", "used");
  EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(style.textAlign, CssTextAlign::Right);
  EXPECT_EQ(parser.resolveStyle("span", "used").textAlign, CssTextAlign::Center);

  ASSERT_TRUE(parser.loadFromCache());
  EXPECT_EQ(parser.ruleCount(), 5u);
  parser.deleteCache();
}

CssFontWeight weightOf(const char* declaration) { return CssParser::parseInlineStyle(declaration).fontWeight; }

TEST(CssFontWeight, SemiboldAndHeavierAreBold) {
  EXPECT_EQ(weightOf("font-weight: 600"), CssFontWeight::Bold);
  EXPECT_EQ(weightOf("font-weight: 700"), CssFontWeight::Bold);
  EXPECT_EQ(weightOf("font-weight: 900"), CssFontWeight::Bold);
  EXPECT_EQ(weightOf("font-weight: bold"), CssFontWeight::Bold);
  EXPECT_EQ(weightOf("font-weight: bolder"), CssFontWeight::Bold);
}

TEST(CssFontWeight, MediumAndLighterAreNormal) {
  EXPECT_EQ(weightOf("font-weight: 500"), CssFontWeight::Normal);
  EXPECT_EQ(weightOf("font-weight: 400"), CssFontWeight::Normal);
  EXPECT_EQ(weightOf("font-weight: 100"), CssFontWeight::Normal);
  EXPECT_EQ(weightOf("font-weight: normal"), CssFontWeight::Normal);
  EXPECT_EQ(weightOf("font-weight: lighter"), CssFontWeight::Normal);
  EXPECT_EQ(weightOf("font-weight: 600px"), CssFontWeight::Normal);  // not a bare number
}

}  // namespace
