// Host tests for siblingOriginPath — the pure helper that derives a device-tagged
// book's untagged sibling path (used to seed per-book stats on first cache create).
//
// Header-only + pure (it only calls KOReaderDocumentId::stripDeviceTag), so the
// test compiles against the header alone — no BookCacheUtils.cpp, no Arduino/HAL.
#include <gtest/gtest.h>

#include <string>

#include "BookCacheUtils.h"

namespace {

TEST(SiblingOriginPath, SuffixTagWithDir) {
  EXPECT_EQ(siblingOriginPath("/calibre/book (X3).epub"), "/calibre/book.epub");
  EXPECT_EQ(siblingOriginPath("/calibre/book (X4).epub"), "/calibre/book.epub");
}

TEST(SiblingOriginPath, PrefixTagWithDir) {
  EXPECT_EQ(siblingOriginPath("/calibre/(X3) book.epub"), "/calibre/book.epub");
}

TEST(SiblingOriginPath, MultiDigitTag) {
  EXPECT_EQ(siblingOriginPath("/a/b/My Title (X12).epub"), "/a/b/My Title.epub");
}

TEST(SiblingOriginPath, NoDirectory) { EXPECT_EQ(siblingOriginPath("book (X3).epub"), "book.epub"); }

TEST(SiblingOriginPath, RealWorldAuthorTitle) {
  EXPECT_EQ(siblingOriginPath("/calibre/Henry Kissinger - From Third World to First (X4).epub"),
            "/calibre/Henry Kissinger - From Third World to First.epub");
}

// Untagged inputs return "" — caller treats that as "this IS the origin, nothing to import".
TEST(SiblingOriginPath, UntaggedReturnsEmpty) {
  EXPECT_EQ(siblingOriginPath("/calibre/book.epub"), "");
  EXPECT_EQ(siblingOriginPath("book.epub"), "");
}

TEST(SiblingOriginPath, NoSeparatorSpaceIsNotATag) {
  // "(X3)" glued to the stem is not the optimizer tag -> untagged -> "".
  EXPECT_EQ(siblingOriginPath("/calibre/book(X3).epub"), "");
}

TEST(SiblingOriginPath, KeepsNonDeviceParens) { EXPECT_EQ(siblingOriginPath("/calibre/book (Annotated).epub"), ""); }

}  // namespace
