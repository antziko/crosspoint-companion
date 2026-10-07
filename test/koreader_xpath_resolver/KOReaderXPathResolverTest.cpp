#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ChapterXPathResolver.h"

namespace {
// crengine only writes the indexed "/body/DocFragment[N]" form when a book has more than
// one spine item, so the shared fixture carries a second, never-resolved chapter.
std::shared_ptr<Epub> epubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  spine.push_back("<html><body><p>Second chapter</p></body></html>");
  return std::make_shared<Epub>(std::move(spine));
}

// One spine item: crengine omits the index entirely and writes "/body/DocFragment/body".
std::shared_ptr<Epub> singleSpineEpubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  return std::make_shared<Epub>(std::move(spine));
}

constexpr char kNestedFixture[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><div><section><p>Alpha bravo</p><p>Second <em>nested</em> tail</p></section></div></body></html>)";

constexpr char kNonVisibleInlineFixture[] =
    R"(<html><body><p><RP><span>hidden</span></RP>Visible text</p></body></html>)";

constexpr char kCommentBoundaryFixture[] = R"(<html><body><p>before<!--comment-->after</p></body></html>)";
constexpr char kProcessingInstructionBoundaryFixture[] = R"(<html><body><p>before<?marker?>after</p></body></html>)";
constexpr char kCdataBoundaryFixture[] = R"(<html><body><p>before<![CDATA[middle]]>after</p></body></html>)";
constexpr char kHiddenCdataFixture[] = R"(<html><body><p>before<rp><![CDATA[hidden]]></rp>after</p></body></html>)";
}  // namespace

TEST(KOReaderXPathResolver, ResolvesExactOffsetWithFullAncestry) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].6");
}

TEST(KOReaderXPathResolver, PreservesNestedInlineTextNode) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 26),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[2].2");
}

TEST(KOReaderXPathResolver, EmitsDetailedAnchorForOffsetZero) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, IgnoresNestedNonVisibleInlineText) {
  const auto epub = epubWith(kNonVisibleInlineFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, ResolvesProgressAfterNestedNonVisibleInlineText) {
  const auto epub = epubWith(kNonVisibleInlineFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f), "/body/DocFragment[1]/body/p[1]/text()[1].12");
}

TEST(KOReaderXPathResolver, CountsUtf8CodepointsInsteadOfBytes) {
  const auto epub = epubWith(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html><body><p>A\xC3\xA9\xE4\xB8\xAD"
      "B</p></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3),
            "/body/DocFragment[1]/body/p[1]/text()[1].3");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundComments) {
  const auto epub = epubWith(kCommentBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/p[1]/text()[2].0");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 11).empty());
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundProcessingInstructions) {
  const auto epub = epubWith(kProcessingInstructionBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundCdata) {
  const auto epub = epubWith(kCdataBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 13),
            "/body/DocFragment[1]/body/p[1]/text()[3].1");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstComment) {
  const auto epub = epubWith(R"(<html><body><p><!--comment-->text</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstProcessingInstruction) {
  const auto epub = epubWith(R"(<html><body><p><?marker?>text</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstCdata) {
  const auto epub = epubWith(R"(<html><body><p><![CDATA[text]]></p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, CountsVisibleCdataAndIgnoresHiddenCdata) {
  const auto visible = epubWith(kCdataBoundaryFixture);
  const auto hidden = epubWith(kHiddenCdataFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(visible, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(hidden, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, ReturnsEmptyForUnusableContent) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(""), 0, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith("<html><body><p>broken"), 0, 100).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(
                  epubWith("<html><body><div>not a paragraph or list item</div></body></html>"), 0, 0)
                  .empty());
}

TEST(KOReaderXPathResolver, KeepsParagraphOnlyResolutionUnchanged) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epub, 0, 2),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]");
}

// --- Single-spine books: crengine omits the DocFragment index ---------------

TEST(KOReaderXPathResolver, OmitsDocFragmentIndexForSingleSpineOffset) {
  const auto epub = singleSpineEpubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment/body/div[1]/section[1]/p[1]/text()[1].6");
}

TEST(KOReaderXPathResolver, OmitsDocFragmentIndexForSingleSpineParagraph) {
  const auto epub = singleSpineEpubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epub, 0, 2), "/body/DocFragment/body/div[1]/section[1]/p[2]");
}

TEST(KOReaderXPathResolver, OmitsDocFragmentIndexForSingleSpineChapterStart) {
  const auto epub = singleSpineEpubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.0f), "/body/DocFragment/body");
}

// --- Batch resolution -------------------------------------------------------

namespace {
constexpr char kManyParagraphFixture[] =
    R"(<html><body><div><p>One</p><p>Two</p></div><section><p>Three</p><li>Four</li><p>Five</p></section></body></html>)";
}  // namespace

TEST(KOReaderXPathResolver, BatchResolvesEveryRequestedParagraph) {
  const auto epub = epubWith(kManyParagraphFixture);
  const uint16_t targets[] = {1, 3, 5};
  std::string out[3];

  EXPECT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 0, targets, out, 3), 3u);
  EXPECT_EQ(out[0], "/body/DocFragment[1]/body/div[1]/p[1]");
  EXPECT_EQ(out[1], "/body/DocFragment[1]/body/section[1]/p[1]");
  EXPECT_EQ(out[2], "/body/DocFragment[1]/body/section[1]/p[2]");
}

// The batch path is the single-paragraph path's implementation, so the two must never
// disagree — that equivalence is what makes it safe to resolve a chapter's bookmarks at once.
TEST(KOReaderXPathResolver, BatchAgreesWithIndividualResolution) {
  const auto epub = epubWith(kManyParagraphFixture);
  const uint16_t targets[] = {1, 2, 3, 4, 5};
  std::string batch[5];
  ASSERT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 0, targets, batch, 5), 5u);

  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(batch[i], ChapterXPathResolver::findXPathForParagraph(epub, 0, targets[i])) << "paragraph " << targets[i];
  }
}

TEST(KOReaderXPathResolver, BatchToleratesUnsortedAndRepeatedTargets) {
  const auto epub = epubWith(kManyParagraphFixture);
  const uint16_t targets[] = {5, 1, 5};
  std::string out[3];

  EXPECT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 0, targets, out, 3), 3u);
  EXPECT_EQ(out[0], "/body/DocFragment[1]/body/section[1]/p[2]");
  EXPECT_EQ(out[1], "/body/DocFragment[1]/body/div[1]/p[1]");
  EXPECT_EQ(out[2], out[0]) << "a repeated target is filled too";
}

TEST(KOReaderXPathResolver, BatchLeavesMissingParagraphsEmpty) {
  const auto epub = epubWith(kManyParagraphFixture);
  const uint16_t targets[] = {2, 99, 0};
  std::string out[3] = {"stale", "stale", "stale"};

  EXPECT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 0, targets, out, 3), 1u);
  EXPECT_EQ(out[0], "/body/DocFragment[1]/body/div[1]/p[2]");
  EXPECT_TRUE(out[1].empty()) << "out of range";
  EXPECT_TRUE(out[2].empty()) << "paragraph indices are 1-based";
}

TEST(KOReaderXPathResolver, BatchRejectsBadArguments) {
  const auto epub = epubWith(kManyParagraphFixture);
  const uint16_t targets[] = {1};
  std::string out[1];

  EXPECT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 0, nullptr, out, 1), 0u);
  EXPECT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 0, targets, nullptr, 1), 0u);
  EXPECT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 0, targets, out, 0), 0u);
  EXPECT_EQ(ChapterXPathResolver::findXPathsForParagraphs(epub, 99, targets, out, 1), 0u) << "spine out of range";
}

// --- batched offset resolution ---------------------------------------------

TEST(KOReaderXPathResolver, BatchedOffsetsAgreeWithIndividualResolution) {
  const auto epub = epubWith(kNestedFixture);

  const uint32_t offsets[] = {0, 6, 26};
  std::string got[3];
  EXPECT_EQ(ChapterXPathResolver::findXPathsForOffsets(epub, 0, offsets, got, 3), 3u);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(got[i], ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offsets[i])) << "offset " << i;
  }
}

TEST(KOReaderXPathResolver, BatchedOffsetsToleratesUnsortedAndRepeatedTargets) {
  const auto epub = epubWith(kNestedFixture);

  const uint32_t offsets[] = {26, 0, 26};
  std::string got[3];
  EXPECT_EQ(ChapterXPathResolver::findXPathsForOffsets(epub, 0, offsets, got, 3), 3u);
  EXPECT_EQ(got[0], "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[2].2");
  EXPECT_EQ(got[1], "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].0");
  EXPECT_EQ(got[2], got[0]);
}

TEST(KOReaderXPathResolver, BatchedOffsetsLeavesOutOfRangeTargetsEmpty) {
  const auto epub = epubWith(kNestedFixture);

  const uint32_t offsets[] = {0, 100000};
  std::string got[2];
  EXPECT_EQ(ChapterXPathResolver::findXPathsForOffsets(epub, 0, offsets, got, 2), 1u);
  EXPECT_FALSE(got[0].empty());
  EXPECT_TRUE(got[1].empty());
}

// --- quote text location ----------------------------------------------------

TEST(KOReaderXPathResolver, LocatesQuoteTextAsAVisibleCodepointRange) {
  const auto epub = epubWith(kNestedFixture);

  const std::string needles[] = {"bravo"};
  ChapterXPathResolver::TextRange ranges[1];
  ASSERT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 1u);
  ASSERT_TRUE(ranges[0].found);
  // "Alpha bravo" — the space is dropped by normalisation but still counts as a visible
  // codepoint, so "bravo" starts at 6 and ends (half-open) at 11.
  EXPECT_EQ(ranges[0].start, 6u);
  EXPECT_EQ(ranges[0].end, 11u);
}

TEST(KOReaderXPathResolver, IgnoresWhitespaceAndDashDifferencesWhenLocating) {
  // The source wraps the phrase across lines and spells the dash "--"; the page's stored
  // text is the layout tokens joined with single spaces, with the dash split away.
  const auto epub = epubWith("<html><body><p>Start\n   east--west\n   finish</p></body></html>");

  const std::string needles[] = {"east west finish"};
  ChapterXPathResolver::TextRange ranges[1];
  ASSERT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 1u);
  EXPECT_TRUE(ranges[0].found);
}

TEST(KOReaderXPathResolver, LocatesTextSpanningTwoTextNodes) {
  const auto epub = epubWith(kNestedFixture);

  // "Second " + <em>"nested"</em> + " tail" — one phrase over three text nodes.
  const std::string needles[] = {"Second nested tail"};
  ChapterXPathResolver::TextRange ranges[1];
  ASSERT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 1u);
  ASSERT_TRUE(ranges[0].found);
  EXPECT_EQ(ranges[0].start, 11u);  // right after "Alpha bravo"
}

TEST(KOReaderXPathResolver, RefusesToAnchorTextThatOccursTwice) {
  const auto epub = epubWith("<html><body><p>repeat me</p><p>repeat me</p></body></html>");

  const std::string needles[] = {"repeat me"};
  ChapterXPathResolver::TextRange ranges[1];
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 0u);
  EXPECT_FALSE(ranges[0].found);
}

TEST(KOReaderXPathResolver, ReportsAbsentTextAsNotFound) {
  const auto epub = epubWith(kNestedFixture);

  const std::string needles[] = {"nowhere in this chapter"};
  ChapterXPathResolver::TextRange ranges[1];
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 0u);
  EXPECT_FALSE(ranges[0].found);
}

TEST(KOReaderXPathResolver, LocatesSeveralNeedlesInOnePass) {
  const auto epub = epubWith(kNestedFixture);

  const std::string needles[] = {"bravo", "", "nested"};
  ChapterXPathResolver::TextRange ranges[3];
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 3), 2u);
  EXPECT_TRUE(ranges[0].found);
  EXPECT_FALSE(ranges[1].found);  // an empty needle is skipped, never matched
  EXPECT_TRUE(ranges[2].found);
}

TEST(KOReaderXPathResolver, SkipsNonVisibleTextWhenLocating) {
  const auto epub = epubWith(kNonVisibleInlineFixture);

  const std::string hidden[] = {"hidden"};
  ChapterXPathResolver::TextRange ranges[1];
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, hidden, ranges, 1), 0u);

  const std::string visible[] = {"Visible text"};
  ASSERT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, visible, ranges, 1), 1u);
  EXPECT_EQ(ranges[0].start, 0u);
}

TEST(KOReaderXPathResolver, LocatedRangeResolvesToBothHighlightAnchors) {
  const auto epub = epubWith(kNestedFixture);

  const std::string needles[] = {"bravo"};
  ChapterXPathResolver::TextRange ranges[1];
  ASSERT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 1u);

  const uint32_t offsets[] = {ranges[0].start, ranges[0].end};
  const bool endOfRange[] = {false, true};
  std::string paths[2];
  ASSERT_EQ(ChapterXPathResolver::findXPathsForOffsets(epub, 0, offsets, paths, 2, endOfRange), 2u);
  EXPECT_EQ(paths[0], "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].6");
  EXPECT_EQ(paths[1], "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].11");
}

TEST(KOReaderXPathResolver, TextRangeRejectsBadArguments) {
  const auto epub = epubWith(kNestedFixture);
  const std::string needles[] = {"bravo"};
  ChapterXPathResolver::TextRange ranges[1];

  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, nullptr, ranges, 1), 0u);
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, nullptr, 1), 0u);
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 0), 0u);
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 9, needles, ranges, 1), 0u);
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(nullptr, 0, needles, ranges, 1), 0u);
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, ChapterXPathResolver::kMaxTextNeedles + 1),
            0u);
}

TEST(KOReaderXPathResolver, RangeEndStaysInsideTheTextNodeItRanThrough) {
  const auto epub = epubWith(kNestedFixture);

  // "Alpha bravo" is 11 codepoints, so the exclusive end of a highlight over the whole
  // paragraph is 11 — which, read plainly, is the first character of the NEXT paragraph.
  const uint32_t offsets[] = {11, 11};
  const bool endOfRange[] = {false, true};
  std::string paths[2];
  ASSERT_EQ(ChapterXPathResolver::findXPathsForOffsets(epub, 0, offsets, paths, 2, endOfRange), 2u);
  EXPECT_EQ(paths[0], "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[1].0");
  EXPECT_EQ(paths[1], "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].11");
}

TEST(KOReaderXPathResolver, LocatesTextStraddlingAParseChunkBoundary) {
  // The chapter is streamed in 1 KiB chunks, so a needle placed either side of the first
  // boundary only matches if the window is retained across chunks.
  std::string filler(1000, 'x');
  const auto epub = epubWith("<html><body><p>" + filler + "needle here tail</p></body></html>");

  const std::string needles[] = {"needle here tail"};
  ChapterXPathResolver::TextRange ranges[1];
  ASSERT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 1u);
  ASSERT_TRUE(ranges[0].found);
  EXPECT_EQ(ranges[0].start, 1000u);
  EXPECT_EQ(ranges[0].end, 1016u);  // "needle here tail" is 16 codepoints including spaces
}

TEST(KOReaderXPathResolver, SkipsANeedleLongerThanTheMatchWindow) {
  const std::string huge(ChapterXPathResolver::kMaxNeedleBytes + 1, 'y');
  const auto epub = epubWith("<html><body><p>" + huge + "</p></body></html>");

  const std::string needles[] = {huge};
  ChapterXPathResolver::TextRange ranges[1];
  EXPECT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 0u);
  EXPECT_FALSE(ranges[0].found);
}

TEST(KOReaderXPathResolver, CountsOnlyVisibleParagraphText) {
  const auto epub = epubWith(kNestedFixture);

  // "Alpha bravo" (11) + "Second " (7) + "nested" (6) + " tail" (5).
  EXPECT_EQ(ChapterXPathResolver::countVisibleChars(epub, 0), 29u);
}

TEST(KOReaderXPathResolver, CountingAnAbsentSpineItemYieldsZero) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::countVisibleChars(epub, 7), 0u);
  EXPECT_EQ(ChapterXPathResolver::countVisibleChars(nullptr, 0), 0u);
}

TEST(KOReaderXPathResolver, ProgressMapsToAOneBasedOffsetInsideTheChapter) {
  EXPECT_EQ(ChapterXPathResolver::offsetForProgress(0.0f, 100), 1u);
  EXPECT_EQ(ChapterXPathResolver::offsetForProgress(0.5f, 100), 50u);
  EXPECT_EQ(ChapterXPathResolver::offsetForProgress(1.0f, 100), 100u);
  // Out-of-range progress clamps rather than running off the end of the chapter.
  EXPECT_EQ(ChapterXPathResolver::offsetForProgress(-1.0f, 100), 1u);
  EXPECT_EQ(ChapterXPathResolver::offsetForProgress(2.0f, 100), 100u);
  // No text means no offset to name.
  EXPECT_EQ(ChapterXPathResolver::offsetForProgress(0.5f, 0), 0u);
}

TEST(KOReaderXPathResolver, BatchedProgressAgreesWithTheSingleTargetApi) {
  const auto epub = epubWith(kNestedFixture);
  const size_t total = ChapterXPathResolver::countVisibleChars(epub, 0);
  ASSERT_GT(total, 0u);

  // Progress 0 is excluded: findXPathForProgress answers the bare fragment path there,
  // which has no offset for findXPathsForOffsets to resolve. Callers handle that case.
  const float progresses[3] = {0.25f, 0.5f, 1.0f};
  uint32_t offsets[3];
  bool inclusive[3];
  for (size_t i = 0; i < 3; i++) {
    offsets[i] = ChapterXPathResolver::offsetForProgress(progresses[i], total);
    inclusive[i] = true;
  }

  std::string batched[3];
  ChapterXPathResolver::findXPathsForOffsets(epub, 0, offsets, batched, 3, inclusive);
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(batched[i], ChapterXPathResolver::findXPathForProgress(epub, 0, progresses[i]))
        << "progress " << progresses[i];
  }
}

// Named HTML entities are not XML: under an XHTML DOCTYPE expat drops them unless a handler
// expands them, so a quote opening on &ldquo; never matched.
TEST(KOReaderXPathResolver, MatchesANamedEntityUnderAnXhtmlDoctype) {
  const auto epub = epubWith(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
      "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>"
      "&ldquo;In Hong Kong, what is not expressly forbidden</p></body></html>");
  const std::string needles[] = {"\xE2\x80\x9CIn Hong Kong, what is not expressly forbidden"};
  ChapterXPathResolver::TextRange ranges[1];
  ASSERT_EQ(ChapterXPathResolver::findTextRanges(epub, 0, needles, ranges, 1), 1u);
  EXPECT_EQ(ranges[0].start, 0u);  // the expanded “ is the first visible codepoint
}

// The offset passes count an expanded entity too, so an offset past one still resolves into
// the text node that holds it.
TEST(KOReaderXPathResolver, CountsAnExpandedEntityAsOneVisibleCodepoint) {
  const auto epub = epubWith(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" "
      "\"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
      "<p>a&mdash;b</p></body></html>");
  EXPECT_EQ(ChapterXPathResolver::countVisibleChars(epub, 0), 3u);
}
