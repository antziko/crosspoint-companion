#include <gtest/gtest.h>

#include <string>

#include "DocFragmentPath.h"

namespace {
// Convenience: the offset just past the prefix, or npos when there is none.
size_t endOf(const std::string& xpath) {
  int index = 0;
  size_t end = 0;
  return DocFragmentPath::find(xpath, index, end) ? end : std::string::npos;
}
}  // namespace

TEST(DocFragmentPath, BuildsIndexedPrefixForMultiSpineBook) {
  EXPECT_EQ(DocFragmentPath::body(0, false), "/body/DocFragment[1]/body");
  EXPECT_EQ(DocFragmentPath::body(7, false), "/body/DocFragment[8]/body");
}

TEST(DocFragmentPath, BuildsIndexlessPrefixForSingleSpineBook) {
  EXPECT_EQ(DocFragmentPath::body(0, true), "/body/DocFragment/body");
}

// Generation and parsing must agree: whatever body() emits, find() must accept and
// resolve back to the same spine item.
TEST(DocFragmentPath, BuildAndParseRoundTrip) {
  for (int spine = 0; spine < 4; spine++) {
    const std::string multi = DocFragmentPath::body(spine, false);
    EXPECT_EQ(DocFragmentPath::index(multi) - 1, spine) << multi;
    EXPECT_TRUE(DocFragmentPath::hasAt(multi, multi.size() - strlen("/body"), "/body"));
  }

  const std::string single = DocFragmentPath::body(0, true);
  EXPECT_EQ(DocFragmentPath::index(single) - 1, 0) << single;
}

TEST(DocFragmentPath, ParsesIndexedForm) {
  int index = 0;
  size_t end = 0;

  ASSERT_TRUE(DocFragmentPath::find("/body/DocFragment[8]/body/div[2]/p[4]", index, end));
  EXPECT_EQ(index, 8);
  EXPECT_EQ(end, strlen("/body/DocFragment[8]"));
}

TEST(DocFragmentPath, ParsesIndexlessSingleSpineForm) {
  int index = 0;
  size_t end = 0;

  ASSERT_TRUE(DocFragmentPath::find("/body/DocFragment/body/div[2]/p[4]", index, end));
  EXPECT_EQ(index, 1) << "an indexless fragment is the book's only spine item";
  EXPECT_EQ(end, strlen("/body/DocFragment"));
}

TEST(DocFragmentPath, ParsesMultiDigitIndex) {
  EXPECT_EQ(DocFragmentPath::index("/body/DocFragment[147]/body/p[1]"), 147);
}

TEST(DocFragmentPath, AcceptsBareFragmentWithNoRemainder) {
  EXPECT_EQ(DocFragmentPath::index("/body/DocFragment[3]"), 3);
  EXPECT_EQ(DocFragmentPath::index("/body/DocFragment"), 1);
}

TEST(DocFragmentPath, RejectsMalformedIndex) {
  EXPECT_EQ(DocFragmentPath::index("/body/DocFragment[]/body"), -1);
  EXPECT_EQ(DocFragmentPath::index("/body/DocFragment[abc]/body"), -1);
  EXPECT_EQ(DocFragmentPath::index("/body/DocFragment[0]/body"), -1) << "fragments are 1-based";
  EXPECT_EQ(DocFragmentPath::index("/body/DocFragment[2/body"), -1) << "unterminated bracket";
}

TEST(DocFragmentPath, RejectsXPathWithoutFragment) {
  EXPECT_EQ(DocFragmentPath::index(""), -1);
  EXPECT_EQ(DocFragmentPath::index("/body/p[1]/text().0"), -1);
}

TEST(DocFragmentPath, ReportsWhereTheBodyRemainderBegins) {
  // Both forms must hand the caller a position that "/body" starts at, so downstream
  // ancestry parsing is identical for single- and multi-spine books.
  const std::string indexed = "/body/DocFragment[2]/body/p[1]";
  const std::string indexless = "/body/DocFragment/body/p[1]";

  EXPECT_EQ(indexed.substr(endOf(indexed)), "/body/p[1]");
  EXPECT_EQ(indexless.substr(endOf(indexless)), "/body/p[1]");
}

TEST(DocFragmentPath, HasAtIsBoundsChecked) {
  const std::string xpath = "/body/DocFragment[1]/body";

  EXPECT_TRUE(DocFragmentPath::hasAt(xpath, endOf(xpath), "/body"));
  EXPECT_FALSE(DocFragmentPath::hasAt(xpath, xpath.size(), "/body")) << "must not read past the end";
  EXPECT_FALSE(DocFragmentPath::hasAt(xpath, xpath.size() + 99, "/body")) << "out-of-range pos must not throw";
}

// A chapter-start xpath skips the spine stream and is taken as offset 0. Getting the
// shape test wrong therefore files a mark, and the reading position, on page 0 of its
// chapter -- silently, because nothing downstream can tell a real 0 from a wrong one.
TEST(DocFragmentPathChapterStart, TheBareFragmentAndItsBodyAreTheStart) {
  EXPECT_TRUE(DocFragmentPath::isChapterStart("/body/DocFragment[16]"));
  EXPECT_TRUE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body"));
  EXPECT_TRUE(DocFragmentPath::isChapterStart("/body/DocFragment[16].0"));
  EXPECT_TRUE(DocFragmentPath::isChapterStart("/body/DocFragment/body"));
}

TEST(DocFragmentPathChapterStart, TheOpeningHeadingIsTheStart) {
  EXPECT_TRUE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/h1/text().0"));
  EXPECT_TRUE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/h2[1]/text().0"));
}

// The bug this test exists for: h2[3] is the THIRD heading of the chapter. It was read as
// the chapter start because only the element's shape was checked, never its index, so a
// bookmark on it opened at page 0 of 57 and its highlight was looked for on the wrong page.
TEST(DocFragmentPathChapterStart, ALaterHeadingIsNotTheStart) {
  EXPECT_FALSE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/h2[3]/text().0"));
  EXPECT_FALSE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/h2[2]/text().0"));
  EXPECT_FALSE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/div[2]/text().0"));
}

TEST(DocFragmentPathChapterStart, AnythingPastTheFirstCharacterIsNotTheStart) {
  EXPECT_FALSE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/h1/text().12"));
  EXPECT_FALSE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/h1/text()[2].0"));
  EXPECT_FALSE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/p[1]/text().0"));
  EXPECT_FALSE(DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/div[2]/ul/li[2]/p/text()[1].52"));
  EXPECT_FALSE(
      DocFragmentPath::isChapterStart("/body/DocFragment[16]/body/table[5]/tbody[1]/tr[5]/td[2]/p[1]/text()[1].0"));
}

TEST(DocFragmentPathChapterStart, ElementIndexDefaultsToOneWhenAbsent) {
  const std::string x = "/body/DocFragment[16]/body/h2/text().0";
  EXPECT_EQ(DocFragmentPath::elementIndex(x, 27, x.rfind("/text()")), 1);
  EXPECT_EQ(DocFragmentPath::textNodeIndex("/body/DocFragment[1]/body/p[2]/text()[3].7"), 3);
  EXPECT_EQ(DocFragmentPath::textNodeIndex("/body/DocFragment[1]/body/p[2]/text().7"), 1);
}
