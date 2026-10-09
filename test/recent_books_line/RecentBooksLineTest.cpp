#include <gtest/gtest.h>

#include "RecentBooksLine.h"

using recentline::Fields;

TEST(RecentBooksLine, RoundTrip) {
  const std::string line = recentline::encode("/Libby/Book.epub", "A Title", "An Author", "/.crosspoint/c.bmp");
  ASSERT_EQ(line.back(), '\n');
  Fields f;
  ASSERT_TRUE(recentline::decode(std::string_view(line).substr(0, line.size() - 1), f));
  EXPECT_EQ(f.path, "/Libby/Book.epub");
  EXPECT_EQ(f.title, "A Title");
  EXPECT_EQ(f.author, "An Author");
  EXPECT_EQ(f.cover, "/.crosspoint/c.bmp");
}

TEST(RecentBooksLine, SeparatorsInFieldsBecomeSpaces) {
  const std::string line = recentline::encode("/a.epub", "Tab\there", "Line\nbreak\r", "");
  EXPECT_EQ(line, "/a.epub\tTab here\tLine break \t\n");
  Fields f;
  ASSERT_TRUE(recentline::decode(std::string_view(line).substr(0, line.size() - 1), f));
  EXPECT_EQ(f.title, "Tab here");
  EXPECT_EQ(f.cover, "");
}

TEST(RecentBooksLine, ShortAndBlankLines) {
  Fields f;
  ASSERT_TRUE(recentline::decode("/only/path.epub", f));
  EXPECT_EQ(f.path, "/only/path.epub");
  EXPECT_EQ(f.title, "");
  EXPECT_EQ(f.cover, "");
  EXPECT_FALSE(recentline::decode("", f));
  EXPECT_FALSE(recentline::decode("\ttitle\tauthor\tcover", f));
  ASSERT_TRUE(recentline::decode("/crlf.epub\tT\tA\tC\r", f));
  EXPECT_EQ(f.cover, "C");
}

TEST(RecentBooksLine, TopFolder) {
  EXPECT_EQ(recentline::topFolder("/calibre/Author/Book.epub"), "calibre");
  EXPECT_EQ(recentline::topFolder("/Libby/Book.epub"), "Libby");
  EXPECT_EQ(recentline::topFolder("/Book.epub"), "");
  EXPECT_EQ(recentline::topFolder("Book.epub"), "");
  EXPECT_EQ(recentline::topFolder("//double/Book.epub"), "double");
}
