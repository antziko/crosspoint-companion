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
std::string swap(const std::string& s) { return KOReaderDocumentId::swapAuthorTitle(s); }

// --- Trailing suffix form ---
TEST(StripDeviceTag, SuffixRemovesX3AndX4) {
  EXPECT_EQ(strip("Book (X4).epub"), "Book.epub");
  EXPECT_EQ(strip("Book (X3).epub"), "Book.epub");
}

// Restricted to the X3/X4 device set: other digits are NOT a device tag and pass through,
// so real titles like "Mac OS X (X11)" are never false-stripped.
TEST(StripDeviceTag, SuffixMultiDigitNotStripped) { EXPECT_EQ(strip("Book (X12).epub"), "Book (X12).epub"); }
TEST(StripDeviceTag, SuffixOtherSingleDigitNotStripped) {
  EXPECT_EQ(strip("Book (X2).epub"), "Book (X2).epub");
  EXPECT_EQ(strip("Book (X5).epub"), "Book (X5).epub");
}

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

TEST(StripDeviceTag, PrefixMultiDigitNotStripped) { EXPECT_EQ(strip("(X12) Book.epub"), "(X12) Book.epub"); }
TEST(StripDeviceTag, PrefixOtherSingleDigitNotStripped) {
  EXPECT_EQ(strip("(X2) Book.epub"), "(X2) Book.epub");
  EXPECT_EQ(strip("(X5) Book.epub"), "(X5) Book.epub");
}

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

// --- swapAuthorTitle: author/title order canonicalization ---
TEST(SwapAuthorTitle, BothOrdersConverge) {
  EXPECT_EQ(swap("Smith - Dune.epub"), swap("Dune - Smith.epub"));
  // Canonical form is the lexicographically-sorted pair.
  EXPECT_EQ(swap("Smith - Dune.epub"), "Dune - Smith.epub");
  EXPECT_EQ(swap("Dune - Smith.epub"), "Dune - Smith.epub");
}

TEST(SwapAuthorTitle, NoSeparatorUnchanged) { EXPECT_EQ(swap("Dune.epub"), "Dune.epub"); }

TEST(SwapAuthorTitle, MultipleSeparatorsUnchanged) {
  EXPECT_EQ(swap("Smith - Dune - Annotated.epub"), "Smith - Dune - Annotated.epub");
}

TEST(SwapAuthorTitle, DegenerateEmptyHalfUnchanged) {
  EXPECT_EQ(swap(" - Dune.epub"), " - Dune.epub");
  EXPECT_EQ(swap("Smith - .epub"), "Smith - .epub");
}

TEST(SwapAuthorTitle, NoExtensionStillSwaps) { EXPECT_EQ(swap("Smith - Dune"), "Dune - Smith"); }

TEST(SwapAuthorTitle, PreservesArbitraryExtension) {
  EXPECT_EQ(swap("Smith - Dune.pdf"), "Dune - Smith.pdf");
}

// --- End-to-end: stripDeviceTag then swapAuthorTitle converge across all combos ---
namespace {
std::string norm(const std::string& s) {
  return KOReaderDocumentId::swapAuthorTitle(KOReaderDocumentId::stripDeviceTag(s));
}
}  // namespace

TEST(NormalizePipeline, AllTagAndOrderCombosConverge) {
  const std::string canonical = norm("Smith - Dune.epub");  // "Dune - Smith.epub"
  // author/title order x prefix/suffix tag x X3/X4 — every combination collapses.
  EXPECT_EQ(norm("Smith - Dune.epub"), canonical);
  EXPECT_EQ(norm("Dune - Smith.epub"), canonical);
  EXPECT_EQ(norm("Smith - Dune (X4).epub"), canonical);
  EXPECT_EQ(norm("(X4) Smith - Dune.epub"), canonical);
  EXPECT_EQ(norm("Dune - Smith (X4).epub"), canonical);
  EXPECT_EQ(norm("(X4) Dune - Smith.epub"), canonical);
  EXPECT_EQ(norm("Smith - Dune (X3).epub"), canonical);
  EXPECT_EQ(norm("(X3) Smith - Dune.epub"), canonical);
  EXPECT_EQ(norm("(X3) Dune - Smith.epub"), canonical);
  EXPECT_EQ(norm("(X4) Dune - Smith (X4).epub"), canonical);  // both tags present
}

}  // namespace
