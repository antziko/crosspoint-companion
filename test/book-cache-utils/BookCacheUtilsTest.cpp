// Host tests for siblingOriginPaths — the pure helper that derives a device-tagged
// book's untagged sibling candidates (used to seed per-book stats on first cache
// create). Candidate 1 is the tag-stripped name in the same author/title order;
// candidate 2 is the order-swapped variant (mirrors KOReader filename-sync).
//
// Header-only + pure (it only calls KOReaderDocumentId::stripDeviceTag /
// swapAuthorTitle), so the test compiles against the header alone — no
// BookCacheUtils.cpp, no Arduino/HAL.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "BookCacheUtils.h"

namespace {

using V = std::vector<std::string>;

TEST(SiblingOriginPaths, SuffixTagWithDir) {
  EXPECT_EQ(siblingOriginPaths("/calibre/book (X3).epub"), V({"/calibre/book.epub"}));
  EXPECT_EQ(siblingOriginPaths("/calibre/book (X4).epub"), V({"/calibre/book.epub"}));
}

TEST(SiblingOriginPaths, PrefixTagWithDir) {
  EXPECT_EQ(siblingOriginPaths("/calibre/(X3) book.epub"), V({"/calibre/book.epub"}));
}

// Only X3/X4 are device tags now — other digits are part of the title, so no sibling.
TEST(SiblingOriginPaths, NonDeviceDigitIsNotATag) {
  EXPECT_EQ(siblingOriginPaths("/a/b/My Title (X12).epub"), V({}));
  EXPECT_EQ(siblingOriginPaths("/a/b/My Title (X2).epub"), V({}));
}

TEST(SiblingOriginPaths, NoDirectory) {
  EXPECT_EQ(siblingOriginPaths("book (X3).epub"), V({"book.epub"}));
}

// Single-word title (no " - ") -> only the same-order candidate, no swap variant.
TEST(SiblingOriginPaths, NoSeparatorNoSwapCandidate) {
  EXPECT_EQ(siblingOriginPaths("/calibre/Dune (X4).epub"), V({"/calibre/Dune.epub"}));
}

// Exactly one " - " -> same-order AND swapped-order candidates, in that order.
TEST(SiblingOriginPaths, AuthorTitleProducesSwapCandidate) {
  EXPECT_EQ(siblingOriginPaths("/calibre/Smith - Dune (X4).epub"),
            V({"/calibre/Smith - Dune.epub", "/calibre/Dune - Smith.epub"}));
  // Opening the swapped-order tagged copy yields the mirror pair.
  EXPECT_EQ(siblingOriginPaths("/calibre/Dune - Smith (X3).epub"),
            V({"/calibre/Dune - Smith.epub", "/calibre/Smith - Dune.epub"}));
}

TEST(SiblingOriginPaths, PrefixTagWithSwapCandidate) {
  EXPECT_EQ(siblingOriginPaths("/calibre/(X4) Smith - Dune.epub"),
            V({"/calibre/Smith - Dune.epub", "/calibre/Dune - Smith.epub"}));
}

TEST(SiblingOriginPaths, RealWorldAuthorTitle) {
  EXPECT_EQ(siblingOriginPaths("/calibre/Henry Kissinger - From Third World to First (X4).epub"),
            V({"/calibre/Henry Kissinger - From Third World to First.epub",
               "/calibre/From Third World to First - Henry Kissinger.epub"}));
}

// Multiple " - " -> tag stripped but order NOT swapped (only one candidate).
TEST(SiblingOriginPaths, MultiSeparatorNoSwap) {
  EXPECT_EQ(siblingOriginPaths("/calibre/A - B - C (X4).epub"), V({"/calibre/A - B - C.epub"}));
}

// Untagged inputs return an empty list — caller treats that as "this IS the origin".
TEST(SiblingOriginPaths, UntaggedReturnsEmpty) {
  EXPECT_EQ(siblingOriginPaths("/calibre/book.epub"), V({}));
  EXPECT_EQ(siblingOriginPaths("book.epub"), V({}));
}

TEST(SiblingOriginPaths, NoSeparatorSpaceIsNotATag) {
  // "(X3)" glued to the stem is not the optimizer tag -> untagged -> empty.
  EXPECT_EQ(siblingOriginPaths("/calibre/book(X3).epub"), V({}));
}

TEST(SiblingOriginPaths, KeepsNonDeviceParens) {
  EXPECT_EQ(siblingOriginPaths("/calibre/book (Annotated).epub"), V({}));
}

}  // namespace
