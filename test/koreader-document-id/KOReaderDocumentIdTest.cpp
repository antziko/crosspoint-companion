// Host tests for KOReaderDocumentId::stripDeviceTag — the pure helper that
// normalizes auto-epub-optimizer device tags (e.g. "(X4) " prefix or " (X4)"
// suffix) out of a filename so optimized copies sync under the same KOReader key
// as the original.
//
// Only the header is compiled here: stripDeviceTag is header-only and pure, so
// no Arduino/HAL deps are pulled in.
#include <gtest/gtest.h>

#include <string>

#include "KOReaderDocumentId.h"

namespace {

std::string strip(const std::string& s) { return KOReaderDocumentId::stripDeviceTag(s); }

// --- Trailing suffix form ---
TEST(StripDeviceTag, SuffixRemovesX3AndX4) {
  EXPECT_EQ(strip("Book (X4).epub"), "Book.epub");
  EXPECT_EQ(strip("Book (X3).epub"), "Book.epub");
}

TEST(StripDeviceTag, SuffixMultiDigit) { EXPECT_EQ(strip("Book (X12).epub"), "Book.epub"); }

TEST(StripDeviceTag, SuffixRequiresDigit) { EXPECT_EQ(strip("Book (X).epub"), "Book (X).epub"); }

TEST(StripDeviceTag, SuffixRequiresLeadingSpace) { EXPECT_EQ(strip("Book(X4).epub"), "Book(X4).epub"); }

TEST(StripDeviceTag, SuffixOnlyTrailingGroupRemoved) {
  EXPECT_EQ(strip("My Book (Annotated) (X4).epub"), "My Book (Annotated).epub");
}

TEST(StripDeviceTag, SuffixNoExtension) { EXPECT_EQ(strip("Book (X4)"), "Book"); }

// --- Leading prefix form (new) ---
TEST(StripDeviceTag, PrefixRemovesX3AndX4) {
  EXPECT_EQ(strip("(X4) Book.epub"), "Book.epub");
  EXPECT_EQ(strip("(X3) Book.epub"), "Book.epub");
}

TEST(StripDeviceTag, PrefixMultiDigit) { EXPECT_EQ(strip("(X12) Book.epub"), "Book.epub"); }

TEST(StripDeviceTag, PrefixRealWorldAuthorTitle) {
  EXPECT_EQ(strip("(X4) Henry Kissinger - From Third World to First.epub"),
            "Henry Kissinger - From Third World to First.epub");
}

TEST(StripDeviceTag, PrefixRequiresDigit) { EXPECT_EQ(strip("(X) Book.epub"), "(X) Book.epub"); }

TEST(StripDeviceTag, PrefixRequiresSeparatorSpace) { EXPECT_EQ(strip("(X4)Book.epub"), "(X4)Book.epub"); }

TEST(StripDeviceTag, PrefixKeepsNonDeviceParens) {
  // A leading parenthetical that is not the device tag stays put.
  EXPECT_EQ(strip("(Annotated) Book.epub"), "(Annotated) Book.epub");
}

// --- Both tags present ---
TEST(StripDeviceTag, BothPrefixAndSuffix) { EXPECT_EQ(strip("(X4) Book (X4).epub"), "Book.epub"); }

// --- Passthrough / edges ---
TEST(StripDeviceTag, UnsuffixedPassthrough) { EXPECT_EQ(strip("Book.epub"), "Book.epub"); }

TEST(StripDeviceTag, KeepsNonDeviceParens) { EXPECT_EQ(strip("My Book (Annotated).epub"), "My Book (Annotated).epub"); }

TEST(StripDeviceTag, PreservesArbitraryExtension) { EXPECT_EQ(strip("Book (X4).pdf"), "Book.pdf"); }

TEST(StripDeviceTag, CollisionRenameEdgeNotUnified) {
  // optimize.py collision rename "{stem}-{index}" — documented edge: ends in a
  // digit, not ")", so it is left as-is and will NOT unify with the original.
  EXPECT_EQ(strip("Book-1.epub"), "Book-1.epub");
}

TEST(StripDeviceTag, EmptyInput) { EXPECT_EQ(strip(""), ""); }

}  // namespace
