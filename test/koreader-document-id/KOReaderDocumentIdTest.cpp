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

TEST(SwapAuthorTitle, PreservesArbitraryExtension) { EXPECT_EQ(swap("Smith - Dune.pdf"), "Dune - Smith.pdf"); }

// --- End-to-end: canonicalFilename converges across all tag placements and orders ---
namespace {
std::string norm(const std::string& s) { return KOReaderDocumentId::canonicalFilename(s); }
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

// A tag on the title rather than on the whole name sits at neither end of the filename,
// so one strip pass cannot see it. Canonicalizing the order brings it to the front.
TEST(NormalizePipeline, TagLeadingEitherComponentConverges) {
  const std::string canonical = norm("Smith - Dune.epub");
  EXPECT_EQ(norm("Smith - (X4) Dune.epub"), canonical);
  EXPECT_EQ(norm("Dune - (X4) Smith.epub"), canonical);
  EXPECT_EQ(norm("Smith - (X3) Dune.epub"), canonical);
  EXPECT_EQ(norm("(X4) Smith - (X4) Dune.epub"), canonical);  // both components tagged
}

// The second swap is load-bearing, not cosmetic: removing the tag changes which component
// sorts first, so a name whose tagged half would lead after stripping must be reordered
// again. Without it "Apple - (X4) Zed" would settle on "Zed - Apple".
TEST(NormalizePipeline, ReorderingAfterTheSecondStripIsApplied) {
  EXPECT_EQ(norm("Apple - (X4) Zed.epub"), "Apple - Zed.epub");
  EXPECT_EQ(norm("Zed - (X4) Apple.epub"), "Apple - Zed.epub");
  EXPECT_EQ(norm("Apple - Zed.epub"), "Apple - Zed.epub");
}

// The extra passes must not widen what counts as a tag.
TEST(NormalizePipeline, NonDeviceTagsStillSurviveBothPasses) {
  EXPECT_EQ(norm("Smith - (X12) Dune.epub"), "(X12) Dune - Smith.epub");
  EXPECT_EQ(norm("(x4) Smith - Dune.epub"), "(x4) Smith - Dune.epub");  // case-sensitive
  EXPECT_EQ(norm("Mac OS X (X11) - Smith.epub"), "Mac OS X (X11) - Smith.epub");
  // Still exactly one " - ": a multi-dash name is left alone, tag or no tag.
  EXPECT_EQ(norm("Smith - (X4) Dune - Extra.epub"), "Smith - (X4) Dune - Extra.epub");
}

// Known gap: a tag trailing the FIRST component is at neither end of the filename and no
// reordering brings it to one. Recognizing it needs a wider tag rule than the narrow one
// stripDeviceTag deliberately uses, so it is documented rather than silently half-handled.
TEST(NormalizePipeline, TagTrailingTheFirstComponentIsNotRecognized) {
  EXPECT_EQ(norm("Author (X4) - Title.epub"), "Author (X4) - Title.epub");
  // ...but the same tag on the LAST component is, via the trailing-suffix rule.
  EXPECT_EQ(norm("Title - Author (X4).epub"), norm("Title - Author.epub"));
}

TEST(NormalizePipeline, IsIdempotent) {
  for (const auto* name : {"Smith - (X4) Dune.epub", "Apple - (X4) Zed.epub", "(X4) Dune.epub",
                           "Author (X4) - Title.epub", "Dune.epub", " - Dune.epub"}) {
    EXPECT_EQ(norm(norm(name)), norm(name)) << name;
  }
}

}  // namespace
