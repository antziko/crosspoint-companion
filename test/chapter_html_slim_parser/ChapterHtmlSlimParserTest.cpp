#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <expat.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(false); }
};

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  const uint8_t linkId = parser.currentFootnoteLinkId;
  ASSERT_NE(linkId, 0u);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_STREQ(footnote.href, expectedHref);
  ASSERT_EQ(parser.currentTextBlock->wordLinkIds.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->wordLinkIds.front(), linkId);
  EXPECT_TRUE(parser.currentTextBlock->linkTargetMatches(linkId, expectedHref));
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

TEST_F(ChapterHtmlSlimParserTest, ParagraphWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, HeaderWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, HeadingBlocksAreNotHyphenated) {
  const XML_Char* noAttributes[] = {nullptr};
  parser.hyphenationEnabled = true;

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "h2", noAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Title ", 6);
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_FALSE(parser.currentTextBlock->isHyphenationEnabled());
  // A line break inside the heading starts another block of the same heading.
  ChapterHtmlSlimParser::startElement(&parser, "br", noAttributes);
  ChapterHtmlSlimParser::endElement(&parser, "br");
  ChapterHtmlSlimParser::characterData(&parser, "More ", 5);
  EXPECT_FALSE(parser.currentTextBlock->isHyphenationEnabled());
  ChapterHtmlSlimParser::endElement(&parser, "h2");

  ChapterHtmlSlimParser::startElement(&parser, "p", noAttributes);
  ChapterHtmlSlimParser::characterData(&parser, "Body ", 5);
  ASSERT_NE(parser.currentTextBlock, nullptr);
  EXPECT_TRUE(parser.currentTextBlock->isHyphenationEnabled());
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.abortParse();
}

TEST_F(ChapterHtmlSlimParserTest, SpanWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before ", 7);
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " After ", 7);

  ASSERT_EQ(parser.currentTextBlock->size(), 2);
  // wordAt(): word text lives in the WordStore arena now, words[] holds handles.
  ASSERT_EQ(parser.currentTextBlock->wordAt(0), "Before");
  ASSERT_EQ(parser.currentTextBlock->wordAt(1), "After");
}

TEST_F(ChapterHtmlSlimParserTest, EmptySpacerSpanSeparatesRunInLabel) {
  // <b>Q:</b><span style="margin-left:0.5em"></span>Did it -- the label must not glue onto "Did".
  const XML_Char* spacer[] = {"style", "margin-left:0.5em", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "b", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Q:", 2);
  ChapterHtmlSlimParser::endElement(&parser, "b");
  ChapterHtmlSlimParser::startElement(&parser, "span", spacer);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, "Did it", 6);

  // "it" is still in the part-word buffer.
  ParsedText* block = parser.currentTextBlock.get();
  ASSERT_NE(block, nullptr);
  ASSERT_EQ(block->size(), 2);
  EXPECT_EQ(block->wordAt(0), "Q:");
  EXPECT_EQ(block->wordAt(1), "Did");
  EXPECT_FALSE(block->wordContinues[1]);
}

TEST_F(ChapterHtmlSlimParserTest, PaddedSpanWithTextStillJoinsWord) {
  // A drop cap with padding keeps its letter attached to the rest of the word.
  const XML_Char* dropCap[] = {"style", "padding-right:2px", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "span", dropCap);
  ChapterHtmlSlimParser::characterData(&parser, "T", 1);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, "he end", 6);

  ParsedText* block = parser.currentTextBlock.get();
  ASSERT_NE(block, nullptr);
  ASSERT_EQ(block->size(), 1);
  EXPECT_EQ(block->wordAt(0), "The");
}

TEST_F(ChapterHtmlSlimParserTest, DivWithHiddenAttributeContentShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

// Each <p> in the cell becomes "<word> line"; returns the distinct line y-positions on the page.
std::vector<int16_t> lineRowsAfterTable(ChapterHtmlSlimParser& parser,
                                        const std::vector<std::vector<const char*>>& cells) {
  parser.beginParse();
  parser.currentTextBlock.reset();
  ChapterHtmlSlimParser::startElement(&parser, "table", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
  for (const auto& cell : cells) {
    ChapterHtmlSlimParser::startElement(&parser, "td", nullptr);
    for (const char* text : cell) {
      ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
      ChapterHtmlSlimParser::characterData(&parser, text, static_cast<int>(strlen(text)));
      ChapterHtmlSlimParser::endElement(&parser, "p");
    }
    ChapterHtmlSlimParser::endElement(&parser, "td");
  }
  ChapterHtmlSlimParser::endElement(&parser, "tr");
  ChapterHtmlSlimParser::endElement(&parser, "table");

  std::vector<int16_t> rows;
  if (!parser.currentPage) return rows;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    if (std::find(rows.begin(), rows.end(), element->yPos) == rows.end()) rows.push_back(element->yPos);
  }
  return rows;
}

TEST_F(ChapterHtmlSlimParserTest, OneCellTableKeepsParagraphBreaks) {
  const auto rows = lineRowsAfterTable(parser, {{"cd home", "mkdir practice", "ls"}});
  EXPECT_EQ(rows.size(), 3u);
}

TEST_F(ChapterHtmlSlimParserTest, LongOneCellTableKeepsParagraphBreaks) {
  // Over MAX_GRID_TABLE_CELL_WORDS, so the row stacks mid-cell and the cell is held to the row end.
  std::vector<const char*> lines(12, "one two three four");
  const auto rows = lineRowsAfterTable(parser, {lines});
  EXPECT_EQ(rows.size(), 12u);
}

TEST_F(ChapterHtmlSlimParserTest, MultiColumnCellParagraphsStayJoined) {
  // Two columns: the first cell's paragraphs still collapse into one line beside the second cell.
  const auto rows = lineRowsAfterTable(parser, {{"aa", "bb"}, {"cc"}});
  EXPECT_EQ(rows.size(), 1u);
}

std::vector<int16_t> lineRows(const ChapterHtmlSlimParser& parser) {
  std::vector<int16_t> rows;
  if (!parser.currentPage) return rows;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    if (std::find(rows.begin(), rows.end(), element->yPos) == rows.end()) rows.push_back(element->yPos);
  }
  return rows;
}

TEST_F(ChapterHtmlSlimParserTest, PreKeepsLineBreaksWithoutParagraphGaps) {
  parser.paragraphSpacing = 5;  // a gap between paragraphs, which code lines must not get
  parser.beginParse();
  parser.currentTextBlock.reset();
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  // Text ending in <br/>, which would otherwise leave a blank line above the code.
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "intro", 5);
  ChapterHtmlSlimParser::startElement(&parser, "br", nullptr);
  ChapterHtmlSlimParser::endElement(&parser, "br");
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::startElement(&parser, "pre", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "code", nullptr);
  // Leading newline is dropped; the blank line is kept; the trailing newline is dropped.
  const char* code = "\nmkdir a\ncd a\n\n  npm init\n";
  ChapterHtmlSlimParser::characterData(&parser, code, static_cast<int>(strlen(code)));
  ChapterHtmlSlimParser::endElement(&parser, "code");
  ChapterHtmlSlimParser::endElement(&parser, "pre");
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "after", 5);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::endElement(&parser, "body");
  parser.makePages();

  auto rows = lineRows(parser);
  ASSERT_EQ(rows.size(), 5u);
  const int introRow = rows.front();
  rows.erase(rows.begin());
  const int step = rows[1] - rows[0];
  EXPECT_GT(step, 0);
  // Only the paragraph gap plus the box's own small margin and padding, no <br/> blank line.
  const int lineHeight = 16;  // stub renderer
  EXPECT_LE(rows[0] - introRow, step + paragraphGapPx(lineHeight, 5) + lineHeight / 2);
  EXPECT_EQ(rows[2] - rows[1], 2 * step);  // blank line between "cd a" and "npm init"
  EXPECT_GT(rows[3] - rows[2], step);      // the paragraph gap returns after </pre>

  // One outline encloses the three code rows and not the paragraph after it.
  const PageElement* box = nullptr;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() == TAG_PageBox) {
      ASSERT_EQ(box, nullptr);
      box = element.get();
    }
  }
  ASSERT_NE(box, nullptr);
  const auto& outline = static_cast<const PageBox&>(*box);
  EXPECT_LT(outline.yPos, rows[0]);
  EXPECT_GT(outline.yPos + outline.getHeight(), rows[2] + step);
  EXPECT_LT(outline.yPos + outline.getHeight(), rows[3]);
}

TEST_F(ChapterHtmlSlimParserTest, PreIndentedLongLineWrapsWithoutBlankLines) {
  parser.beginParse();
  parser.currentTextBlock.reset();
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "pre", nullptr);
  std::string code = "a {\n";
  code += std::string(12, ' ');
  code += std::string(400, 'x');  // one word far wider than the line
  code += " = y;\nb\n";
  ChapterHtmlSlimParser::characterData(&parser, code.c_str(), static_cast<int>(code.size()));
  ChapterHtmlSlimParser::endElement(&parser, "pre");
  ChapterHtmlSlimParser::endElement(&parser, "body");
  parser.makePages();

  const auto rows = lineRows(parser);
  ASSERT_GE(rows.size(), 4u);
  const int step = rows[1] - rows[0];
  for (size_t i = 2; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i] - rows[i - 1], step) << "gap before row " << i;
  }
}

// The alignment of the cell opened by <table style=tableStyle><tr><td style=cellStyle>.
CssTextAlign tableCellAlignment(ChapterHtmlSlimParser& parser, const CssTextAlign readerAlign, const char* tableStyle,
                                const char* cellStyle) {
  parser.paragraphAlignment = static_cast<uint8_t>(readerAlign);
  const XML_Char* tableAtts[] = {"style", tableStyle, nullptr};
  const XML_Char* cellAtts[] = {"style", cellStyle, nullptr};
  parser.inlineStyleStack.clear();  // beginParse expects a fresh parser
  parser.currentCssStyle.reset();
  parser.updateEffectiveInlineStyle();
  parser.beginParse();
  parser.currentTextBlock.reset();
  ChapterHtmlSlimParser::startElement(&parser, "table", tableStyle ? tableAtts : nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "tr", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "td", cellStyle ? cellAtts : nullptr);
  return parser.currentTextBlock ? parser.currentTextBlock->getBlockStyle().alignment : CssTextAlign::None;
}

TEST_F(ChapterHtmlSlimParserTest, ReaderAlignmentReplacesBookJustifyInTableCell) {
  EXPECT_EQ(tableCellAlignment(parser, CssTextAlign::Left, nullptr, "text-align: justify"), CssTextAlign::Left);
  EXPECT_EQ(tableCellAlignment(parser, CssTextAlign::Right, "text-align: justify", nullptr), CssTextAlign::Right);
}

TEST_F(ChapterHtmlSlimParserTest, TableCellKeepsBookAlignmentOtherwise) {
  EXPECT_EQ(tableCellAlignment(parser, CssTextAlign::Left, nullptr, "text-align: center"), CssTextAlign::Center);
  EXPECT_EQ(tableCellAlignment(parser, CssTextAlign::Justify, nullptr, "text-align: justify"), CssTextAlign::Justify);
  EXPECT_EQ(tableCellAlignment(parser, CssTextAlign::None, nullptr, "text-align: justify"), CssTextAlign::Justify);
  EXPECT_EQ(tableCellAlignment(parser, CssTextAlign::Justify, nullptr, nullptr), CssTextAlign::Left);
}

// Text running straight into a block ("one<div>two</div>") belongs to the paragraph before it.
class BlockStartTest : public ChapterHtmlSlimParserTest {};

TEST_P(BlockStartTest, TextBeforeBlockStaysInItsOwnParagraph) {
  const char* blockTag = GetParam();
  parser.beginParse();
  parser.currentTextBlock.reset();
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "div", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "one", 3);
  ChapterHtmlSlimParser::startElement(&parser, blockTag, nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "two", 3);
  ChapterHtmlSlimParser::endElement(&parser, blockTag);
  ChapterHtmlSlimParser::endElement(&parser, "div");
  ChapterHtmlSlimParser::endElement(&parser, "body");
  parser.makePages();

  EXPECT_EQ(lineRows(parser).size(), 2u);
}

INSTANTIATE_TEST_SUITE_P(BlockStarts, BlockStartTest, ::testing::Values("div", "p", "h3"));

// CSS `inherit` on font-weight/font-style keeps the parent's emphasis instead of resetting it.
// The parent's emphasis comes from CSS, not <b>/<i>, which are tracked separately by depth.
uint8_t styleOfInnerWord(ChapterHtmlSlimParser& parser, const char* outerStyle, const char* innerStyle) {
  const XML_Char* outer[] = {"style", outerStyle, nullptr};
  const XML_Char* inner[] = {"style", innerStyle, nullptr};
  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "span", outer);
  ChapterHtmlSlimParser::startElement(&parser, "span", inner);
  ChapterHtmlSlimParser::characterData(&parser, "word ", 5);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::endElement(&parser, "span");
  if (parser.currentTextBlock->size() == 0) return 0xFF;
  return static_cast<uint8_t>(parser.currentTextBlock->getWordStyleAt(0));
}

TEST_F(ChapterHtmlSlimParserTest, FontWeightInheritKeepsParentBold) {
  const uint8_t style = styleOfInnerWord(parser, "font-weight: bold", "font-weight: inherit");
  ASSERT_NE(style, 0xFF);
  EXPECT_NE(style & EpdFontFamily::BOLD, 0);
}

TEST_F(ChapterHtmlSlimParserTest, FontStyleInheritKeepsParentItalic) {
  const uint8_t style = styleOfInnerWord(parser, "font-style: italic", "font-style: inherit");
  ASSERT_NE(style, 0xFF);
  EXPECT_NE(style & EpdFontFamily::ITALIC, 0);
}

TEST_F(ChapterHtmlSlimParserTest, FontWeightNormalStillOverridesParentBold) {
  const uint8_t style = styleOfInnerWord(parser, "font-weight: bold", "font-weight: normal");
  ASSERT_NE(style, 0xFF);
  EXPECT_EQ(style & EpdFontFamily::BOLD, 0);
}

}  // namespace

// ---- Anchor placement ----
// A one-line viewport (the stub's line height is 16) makes every laid-out line its own page, so an
// anchor's page is the index of the line it sits on.
struct AnchorParse {
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  std::string path = "anchors.xhtml";
  int pagesCompleted = 0;
  std::unique_ptr<ChapterHtmlSlimParser> parser;

  explicit AnchorParse(const std::string& body, std::vector<std::string> tocAnchors = {}) {
    parser = std::make_unique<ChapterHtmlSlimParser>(
        nullptr, path, renderer, 0, 1.0f, false, 0, static_cast<uint16_t>(renderer.getScreenWidth()), 16, false, false,
        [this](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t) { pagesCompleted++; }, true, "", "", 0,
        std::move(tocAnchors), nullptr, &css);
    const std::string html = "<html><body>" + body + "</body></html>";
    parser->beginParse();
    XML_Parser xml = XML_ParserCreate(nullptr);
    XML_SetUserData(xml, parser.get());
    XML_SetElementHandler(xml, ChapterHtmlSlimParser::startElement, ChapterHtmlSlimParser::endElement);
    XML_SetCharacterDataHandler(xml, ChapterHtmlSlimParser::characterData);
    EXPECT_NE(XML_Parse(xml, html.data(), static_cast<int>(html.size()), 1), XML_STATUS_ERROR)
        << XML_ErrorString(XML_GetErrorCode(xml));
    XML_ParserFree(xml);
    parser->xmlParser_ = nullptr;  // freed above; finishParse must not free it again
    parser->finishParse();
  }

  int pageOf(const char* id) const {
    for (const auto& [key, page] : parser->getAnchors()) {
      if (key == id) return page;
    }
    return -1;
  }
};

// n four-letter words, with `<marker>` inserted before each listed word index.
std::string wordsWithMarkers(const int n, const std::vector<std::pair<int, std::string>>& markers) {
  std::string out;
  for (int i = 0; i < n; i++) {
    for (const auto& [at, marker] : markers) {
      if (at == i) out += marker;
    }
    out += "wwww ";
  }
  return out;
}

TEST(ChapterHtmlSlimParserAnchors, EmptySpanMarkerLandsOnItsLine) {
  AnchorParse p(
      "<p>" +
      wordsWithMarkers(40, {{2, "<span id=\"early\"/>"}, {20, "<span id=\"mid\"/>"}, {39, "<span id=\"late\"/>"}}) +
      "</p>");
  const int lines = p.pagesCompleted;
  ASSERT_GE(lines, 3);
  EXPECT_EQ(p.pageOf("early"), 0);
  EXPECT_GT(p.pageOf("mid"), 0);
  EXPECT_LT(p.pageOf("mid"), lines - 1);
  EXPECT_EQ(p.pageOf("late"), lines - 1);
}

TEST(ChapterHtmlSlimParserAnchors, InlineAnchorElementLandsOnItsLine) {
  AnchorParse p("<p>" + wordsWithMarkers(40, {{39, "<a id=\"late\"></a>"}}) + "</p>");
  EXPECT_EQ(p.pageOf("late"), p.pagesCompleted - 1);
}

TEST(ChapterHtmlSlimParserAnchors, SpanWrappingTextIsNotAnAnchor) {
  AnchorParse p("<p>one <span id=\"kobo.1.1\">two</span> three<span id=\"ws\"> </span></p>");
  EXPECT_EQ(p.pageOf("kobo.1.1"), -1);
  EXPECT_EQ(p.pageOf("ws"), 0);  // whitespace-only content still counts as empty
}

TEST(ChapterHtmlSlimParserAnchors, MarkerEndingParagraphLandsOnNextBlock) {
  AnchorParse p("<p>first <span id=\"end\"/></p><p>second</p>");
  ASSERT_EQ(p.pagesCompleted, 2);
  EXPECT_EQ(p.pageOf("end"), 1);
}

TEST(ChapterHtmlSlimParserAnchors, NestedEmptySpansRecordOuterOnly) {
  AnchorParse p("<p>one <span id=\"outer\"><span id=\"inner\"/></span>two</p>");
  EXPECT_EQ(p.pageOf("outer"), 0);
  EXPECT_EQ(p.pageOf("inner"), -1);
}

TEST(ChapterHtmlSlimParserAnchors, GridRowAnchorsUseTheRowsPage) {
  AnchorParse p(
      "<table><tr><td id=\"r1\">1</td><td>first note</td></tr>"
      "<tr><td id=\"r2\">2</td><td><span id=\"s2\"/>second note</td></tr></table>");
  ASSERT_EQ(p.pagesCompleted, 2);
  EXPECT_EQ(p.pageOf("r1"), 0);
  EXPECT_EQ(p.pageOf("r2"), 1);
  EXPECT_EQ(p.pageOf("s2"), 1);
}

TEST(ChapterHtmlSlimParserAnchors, MarkerGluedToWordLandsOnThatWord) {
  // history.<span id/><a>1</a>: a footnote back-link target sits on the reference's line.
  AnchorParse p("<p>first<span id=\"ref\"/></p><p>second</p>");
  EXPECT_EQ(p.pageOf("ref"), 0);
}

TEST(ChapterHtmlSlimParserAnchors, TocSpanWrappingTextIsStillRecorded) {
  AnchorParse p("<p>intro</p><p><span id=\"toc1\">Chapter</span> text</p>", {"toc1"});
  EXPECT_GE(p.pageOf("toc1"), 1);
}

TEST(ChapterHtmlSlimParserAnchors, BlockIdLandsOnPageOfItsFirstLine) {
  AnchorParse p("<p>intro</p><p id=\"p2\">body</p>");
  ASSERT_EQ(p.pagesCompleted, 2);
  EXPECT_EQ(p.pageOf("p2"), 1);
}

TEST(ChapterHtmlSlimParserAnchors, AnchorsSurviveAnEndOfChapterTable) {
  AnchorParse p("<p>text <span id=\"m\"/>more</p><table><tr><td id=\"n1\">1</td><td>note</td></tr></table>");
  EXPECT_EQ(p.pageOf("m"), 0);
  EXPECT_EQ(p.pageOf("n1"), 1);
}

TEST(ChapterHtmlSlimParserAnchors, StackedRowAnchorLandsOnItsFirstLine) {
  // The note's text overflows the grid cell limit, so the row falls back to stacked cells
  // after its anchor was queued; "1" then opens the next page.
  AnchorParse p("<p>intro</p><table><tr><td id=\"n1\">1</td><td>" + wordsWithMarkers(40, {}) + "</td></tr></table>");
  ASSERT_GE(p.pagesCompleted, 3);
  EXPECT_EQ(p.pageOf("n1"), 1);
}

namespace {
// Footnote entries a chapter's pages carry, laid out with images suppressed (no image file reads).
std::vector<FootnoteEntry> footnotesOf(const std::string& body) {
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  std::vector<FootnoteEntry> out;
  ChapterHtmlSlimParser parser(
      nullptr, "notes.xhtml", renderer, 0, 1.0f, false, 0, static_cast<uint16_t>(renderer.getScreenWidth()), 400, false,
      false,
      [&out](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
        for (const auto& f : page->footnotes) out.push_back(f);
      },
      true, "", "", /*imageRendering=*/2, {}, nullptr, &css);
  const std::string html = "<html><body>" + body + "</body></html>";
  parser.beginParse();
  XML_Parser xml = XML_ParserCreate(nullptr);
  XML_SetUserData(xml, &parser);
  XML_SetElementHandler(xml, ChapterHtmlSlimParser::startElement, ChapterHtmlSlimParser::endElement);
  XML_SetCharacterDataHandler(xml, ChapterHtmlSlimParser::characterData);
  EXPECT_NE(XML_Parse(xml, html.data(), static_cast<int>(html.size()), 1), XML_STATUS_ERROR);
  XML_ParserFree(xml);
  parser.xmlParser_ = nullptr;
  parser.finishParse();
  return out;
}
}  // namespace

TEST(ChapterHtmlSlimParserLinkIcons, IconOnlyLinkBecomesNumberedMarker) {
  const auto notes = footnotesOf(
      "<p>spell<sup><a href=\"#footnote1\"><img src=\"icon.jpg\"/>&#13;</a></sup> and more"
      "<sup><a href=\"#footnote2\"><img src=\"icon.jpg\"/></a></sup> text</p>");
  ASSERT_EQ(notes.size(), 2u);
  EXPECT_STREQ(notes[0].number, "1");
  EXPECT_STREQ(notes[0].href, "#footnote1");
  EXPECT_STREQ(notes[1].number, "2");
  EXPECT_STREQ(notes[1].href, "#footnote2");
}

TEST(ChapterHtmlSlimParserLinkIcons, LinkWithTextKeepsItsOwnLabel) {
  const auto notes = footnotesOf("<p>word<a href=\"#n7\">7<img src=\"icon.jpg\"/></a> and <a href=\"#n8\">8</a></p>");
  ASSERT_EQ(notes.size(), 2u);
  EXPECT_STREQ(notes[0].number, "7");
  EXPECT_STREQ(notes[1].number, "8");
}

// ---- Page-foot line carrying and CSS page breaks ----
// A viewport `lines` lines tall (the stub's line height is 16). This suite's TextBlock stub keeps
// no text, so each line is recorded by its block's alignment: 'C' for a (centred) heading, 'L'
// for any other line. The reader's paragraph alignment replaces a book's, so only headings stand out.
struct PageParse {
  GfxRenderer renderer;
  CssParser css{"/tmp"};
  std::string path = "pages.xhtml";
  std::vector<std::string> pages;
  // Per page, each PageBorderBox as "[TRBLS]" with '-' for an open side or no shade, in element
  // order; boxFirst records that they come ahead of the page's other elements.
  std::vector<std::string> boxes;
  std::vector<int> boxHeights;
  std::vector<int> linePts;              // each line's own built-in size, 0 = the section font
  std::vector<std::vector<int>> lineYs;  // per page, each line's y
  bool boxFirst = true;
  std::unique_ptr<ChapterHtmlSlimParser> parser;

  // `lines` sets the page height in 16 px stub lines, unless `heightPx` gives it in pixels.
  PageParse(const std::string& body, const int lines, const int fontId = 0, const bool embeddedStyle = true,
            const uint8_t paragraphSpacing = 0, const int heightPx = 0) {
    parser = std::make_unique<ChapterHtmlSlimParser>(
        nullptr, path, renderer, fontId, 1.0f, paragraphSpacing, 0, static_cast<uint16_t>(renderer.getScreenWidth()),
        static_cast<uint16_t>(heightPx > 0 ? heightPx : lines * 16), false, false,
        [this](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
          std::string tags;
          std::vector<int> ys;
          std::string pageBoxes;
          bool sawOther = false;
          for (const auto& element : page->elements) {
            if (element->getTag() == TAG_PageBorderBox) {
              const auto& box = static_cast<const PageBorderBox&>(*element);
              if (sawOther) boxFirst = false;
              pageBoxes += '[';
              const char names[] = "TRBL";
              for (int i = 0; i < 4; ++i) pageBoxes += box.side(i).visible() ? names[i] : '-';
              pageBoxes += box.isShaded() ? 'S' : '-';
              pageBoxes += ']';
              boxHeights.push_back(box.getHeight());
              continue;
            }
            sawOther = true;
            if (element->getTag() != TAG_PageLine) continue;
            ys.push_back(element->yPos);
            const auto& lineStyle = static_cast<const PageLine&>(*element).getBlock()->getBlockStyle();
            linePts.push_back(lineStyle.fontPt);
            const auto align = lineStyle.alignment;
            tags += align == CssTextAlign::Center ? 'C' : 'L';
          }
          pages.push_back(std::move(tags));
          lineYs.push_back(std::move(ys));
          boxes.push_back(std::move(pageBoxes));
        },
        embeddedStyle, "", "", 0, std::vector<std::string>{}, nullptr, &css);
    const std::string html = "<html><body>" + body + "</body></html>";
    parser->beginParse();
    XML_Parser xml = XML_ParserCreate(nullptr);
    XML_SetUserData(xml, parser.get());
    XML_SetElementHandler(xml, ChapterHtmlSlimParser::startElement, ChapterHtmlSlimParser::endElement);
    XML_SetCharacterDataHandler(xml, ChapterHtmlSlimParser::characterData);
    EXPECT_NE(XML_Parse(xml, html.data(), static_cast<int>(html.size()), 1), XML_STATUS_ERROR)
        << XML_ErrorString(XML_GetErrorCode(xml));
    XML_ParserFree(xml);
    parser->xmlParser_ = nullptr;  // freed above; finishParse must not free it again
    parser->finishParse();
  }
};

// How many four-letter words fill exactly one line.
int wordsPerLine() {
  static const int cached = [] {
    for (int n = 2; n < 200; n++) {
      AnchorParse p("<p>" + wordsWithMarkers(n, {}) + "</p>");
      if (p.pagesCompleted > 1) return n - 1;
    }
    return 0;
  }();
  return cached;
}

// A paragraph exactly `lines` lines long.
std::string paragraph(const int lines) { return "<p>" + wordsWithMarkers(lines * wordsPerLine(), {}) + "</p>"; }

// Stub lines are 16 px; spacing level 3 puts an 8 px gap after each paragraph, which may tighten to
// 6 or widen to 16.
TEST(ChapterHtmlSlimParserPagination, ALoneFirstLineMovesWhenTheGapsHideTheHole) {
  ASSERT_GT(wordsPerLine(), 1);
  // Three one-line paragraphs, then one whose first line would be its only one on the page; the
  // two gaps above the last kept line widen by 8 each to fill the 16 px it leaves.
  PageParse p(paragraph(1) + paragraph(1) + paragraph(1) + paragraph(2), 0, 0, true, 3, 96);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLL");
  EXPECT_EQ(p.lineYs[0], (std::vector<int>{0, 32, 64}));
  EXPECT_EQ(p.pages[1], "LL");
}

TEST(ChapterHtmlSlimParserPagination, ALoneFirstLineStaysWhenNothingCanHideTheHole) {
  ASSERT_GT(wordsPerLine(), 1);
  // No gap has text below it once the line would move, so the page fills instead.
  PageParse p(paragraph(3) + paragraph(2), 0, 0, true, 3, 80);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLLL");
  EXPECT_EQ(p.pages[1], "L");
}

TEST(ChapterHtmlSlimParserPagination, ALoneLastLineTakesALineWithItWhenTheGapsHideTheHole) {
  ASSERT_GT(wordsPerLine(), 1);
  // The fourth paragraph's last line would open page 2 alone; its third line goes with it and
  // the three gaps above widen by 8 each.
  PageParse p(paragraph(1) + paragraph(1) + paragraph(1) + paragraph(4), 0, 0, true, 3, 128);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLLLL");
  EXPECT_EQ(p.lineYs[0], (std::vector<int>{0, 32, 64, 96, 112}));
  EXPECT_EQ(p.pages[1], "LL");
}

TEST(ChapterHtmlSlimParserPagination, OneLongParagraphFillsThePage) {
  ASSERT_GT(wordsPerLine(), 1);
  // No paragraph gaps at all: a lone last line opens page 2 rather than leave a hole.
  PageParse p(paragraph(1) + paragraph(4), 4);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLLL");
  EXPECT_EQ(p.pages[1], "L");
}

TEST(ChapterHtmlSlimParserPagination, AHeadingStaysWithItsText) {
  ASSERT_GT(wordsPerLine(), 1);
  PageParse p(paragraph(3) + "<h2>Head</h2>" + paragraph(3), 4);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLL");
  EXPECT_EQ(p.pages[1].substr(0, 2), "CL");
}

TEST(ChapterHtmlSlimParserPagination, AFullPageParagraphStillBreaks) {
  ASSERT_GT(wordsPerLine(), 1);
  // Nothing to keep together: the page simply fills.
  PageParse p(paragraph(6), 4);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLLL");
  EXPECT_EQ(p.pages[1], "LL");
}

TEST(ChapterHtmlSlimParserPagination, ALineThatMissesByLittleFitsByTighteningGaps) {
  ASSERT_GT(wordsPerLine(), 1);
  // 2 lines, gap, 3 lines need 88 px; the page has 86, so the last line misses by 2.
  PageParse p(paragraph(2) + paragraph(3), 0, 0, true, 3, 86);
  ASSERT_EQ(p.pages.size(), 1u);
  EXPECT_EQ(p.pages[0], "LLLLL");
  EXPECT_EQ(p.lineYs[0], (std::vector<int>{0, 16, 38, 54, 70}));
}

TEST(ChapterHtmlSlimParserPagination, ALastLineStaysAloneWhenTheGapsCannotHideTwoLines) {
  ASSERT_GT(wordsPerLine(), 1);
  // Moving the third paragraph's two lines would leave 32 px; the one gap with text below it can
  // only widen by 8, so the page keeps them and the last line opens page 2.
  PageParse p(paragraph(1) + paragraph(1) + paragraph(3), 0, 0, true, 3, 90);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLLL");
  EXPECT_EQ(p.pages[1], "L");
}

TEST(ChapterHtmlSlimParserPagination, CssPageBreakBeforeStartsAPage) {
  PageParse p("<p>one</p><p style=\"page-break-before: always\">two</p><p>three</p>", 8);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "L");
  EXPECT_EQ(p.pages[1], "LL");
}

TEST(ChapterHtmlSlimParserPagination, CssBreakAfterStartsTheNextBlockOnAPage) {
  PageParse p("<div style=\"break-after: page\"><p>one</p></div><p>two</p>", 8);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "L");
  EXPECT_EQ(p.pages[1], "L");
}

TEST(ChapterHtmlSlimParserPagination, AutoPageBreaksDoNotBreak) {
  PageParse p("<p>one</p><p style=\"page-break-before: auto\">two</p>", 8);
  EXPECT_EQ(p.pages.size(), 1u);
}

TEST(ChapterHtmlSlimParserPagination, AFirstBlockWithAPageBreakAddsNoBlankPage) {
  PageParse p("<p style=\"page-break-before: always\">one</p><p>two</p>", 8);
  ASSERT_EQ(p.pages.size(), 1u);
  EXPECT_EQ(p.pages[0], "LL");
}

TEST(ChapterHtmlSlimParserPagination, AnAnchorOnACarriedLineMovesWithIt) {
  ASSERT_GT(wordsPerLine(), 1);
  // The fourth paragraph's lone first line moves, and its id must name the page it now opens.
  PageParse p(paragraph(1) + paragraph(1) + paragraph(1) + "<p id=\"second\">" +
                  wordsWithMarkers(2 * wordsPerLine(), {}) + "</p>",
              0, 0, true, 3, 96);
  ASSERT_EQ(p.pages.size(), 2u);
  int page = -1;
  for (const auto& [key, at] : p.parser->getAnchors()) {
    if (key == "second") page = at;
  }
  EXPECT_EQ(page, 1);
}

// ---- CSS borders and shading ----
TEST(CssBorders, ShorthandSetsEverySide) {
  const CssStyle style = CssParser::parseInlineStyle("border: 1px solid #000");
  EXPECT_TRUE(style.hasVisibleBorder());
  for (const CssBorderSide* side : {&style.borderTop, &style.borderRight, &style.borderBottom, &style.borderLeft}) {
    EXPECT_EQ(side->width, 1);
    EXPECT_EQ(side->style, CssBorderStyle::Solid);
  }
}

TEST(CssBorders, SideWidthAndStyleCombine) {
  const CssStyle style = CssParser::parseInlineStyle("border-left-style: dashed; border-left-width: thick");
  EXPECT_EQ(style.borderLeft.width, 3);
  EXPECT_EQ(style.borderLeft.style, CssBorderStyle::Dashed);
  EXPECT_FALSE(style.borderTop.visible());
}

TEST(CssBorders, EdgeValuesFollowTheShorthandRule) {
  const CssStyle style = CssParser::parseInlineStyle("border-style: solid none; border-width: 2px");
  EXPECT_TRUE(style.borderTop.visible());
  EXPECT_FALSE(style.borderRight.visible());
  EXPECT_TRUE(style.borderBottom.visible());
  EXPECT_FALSE(style.borderLeft.visible());
}

TEST(CssBorders, WidthWithoutStyleDrawsNothing) {
  EXPECT_FALSE(CssParser::parseInlineStyle("border-width: 2px").hasVisibleBorder());
  EXPECT_FALSE(CssParser::parseInlineStyle("border: none").hasVisibleBorder());
  EXPECT_FALSE(CssParser::parseInlineStyle("border-collapse: collapse").hasVisibleBorder());
}

TEST(CssBorders, InheritedRulesMergeSideParts) {
  CssStyle base = CssParser::parseInlineStyle("border-top-style: solid");
  base.applyOver(CssParser::parseInlineStyle("border-top-width: 2px"));
  EXPECT_EQ(base.borderTop.width, 2);
  EXPECT_EQ(base.borderTop.style, CssBorderStyle::Solid);
}

TEST(CssShade, LightBackgroundsShadeOthersDoNot) {
  EXPECT_TRUE(CssParser::parseInlineStyle("background-color: #ddd").isShaded());
  EXPECT_TRUE(CssParser::parseInlineStyle("background: lightgray").isShaded());
  EXPECT_TRUE(CssParser::parseInlineStyle("background-color: rgb(200, 200, 200)").isShaded());
  EXPECT_FALSE(CssParser::parseInlineStyle("background-color: #fff").isShaded());
  EXPECT_FALSE(CssParser::parseInlineStyle("background-color: black").isShaded());
  EXPECT_FALSE(CssParser::parseInlineStyle("background: none").isShaded());
}

TEST(CssStylePool, ListStyleTypeKeepsStylesApart) {
  // Identical except for the bullet: pooling them together would give one list the other's marker.
  EXPECT_FALSE(CssParser::parseInlineStyle("list-style-type: none") ==
               CssParser::parseInlineStyle("list-style-type: disc"));
}

TEST(ChapterHtmlSlimParserBoxes, ABorderedBlockGetsOneClosedBox) {
  PageParse p("<p>before</p><div style=\"border: 1px solid\"><p>inside</p></div><p>after</p>", 12);
  ASSERT_EQ(p.pages.size(), 1u);
  EXPECT_EQ(p.boxes[0], "[TRBL-]");
  EXPECT_TRUE(p.boxFirst);
  ASSERT_EQ(p.boxHeights.size(), 1u);
  EXPECT_GE(p.boxHeights[0], 16 + 2 * 5);  // the line plus the frame gap and border on both edges
}

TEST(ChapterHtmlSlimParserBoxes, ShadingAloneDrawsAShadedBoxWithoutSides) {
  PageParse p("<div style=\"background-color: #e0e0e0\"><p>inside</p></div>", 12);
  ASSERT_EQ(p.pages.size(), 1u);
  EXPECT_EQ(p.boxes[0], "[----S]");
}

TEST(ChapterHtmlSlimParserBoxes, APageBreakLeavesTheCutEdgesOpen) {
  ASSERT_GT(wordsPerLine(), 1);
  PageParse p("<div style=\"border: 1px solid\">" + paragraph(10) + "</div>", 4);
  ASSERT_GE(p.pages.size(), 3u);
  EXPECT_EQ(p.boxes.front(), "[TR-L-]");
  for (size_t i = 1; i + 1 < p.boxes.size(); ++i) EXPECT_EQ(p.boxes[i], "[-R-L-]") << "page " << i;
  EXPECT_EQ(p.boxes.back(), "[-RBL-]");
}

TEST(ChapterHtmlSlimParserBoxes, AnEmptyBorderedBlockIsARule) {
  PageParse p("<p>one</p><div style=\"border-top: 1px solid\"></div><p>two</p>", 12);
  ASSERT_EQ(p.pages.size(), 1u);
  EXPECT_EQ(p.boxes[0], "[T----]");
}

TEST(ChapterHtmlSlimParserBoxes, NestingDrawsAtMostTwoBoxes) {
  PageParse p(
      "<div style=\"border: 1px solid\"><div style=\"border: 1px solid\"><div style=\"border: 1px solid\">"
      "<p>deep</p></div></div></div>",
      16);
  ASSERT_EQ(p.pages.size(), 1u);
  EXPECT_EQ(p.boxes[0], "[TRBL-][TRBL-]");
}

TEST(ChapterHtmlSlimParserBoxes, ALineCarriedOutOfABoxTakesTheBoxWithIt) {
  ASSERT_GT(wordsPerLine(), 1);
  // Top and bottom rules only, so the text keeps its width. 1px + 4px gap above the text: four
  // lines fit, so the heading would sit alone at the foot and moves with its text (a lone
  // paragraph line in a box stays: a box page cannot hide the hole). The box must end where the
  // heading used to start.
  PageParse p("<div style=\"border-top: 1px solid; border-bottom: 1px solid\">" + paragraph(3) + "<h2>Head</h2>" +
                  paragraph(3) + "</div>",
              5);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLL");
  EXPECT_EQ(p.pages[1].substr(0, 2), "CL");
  EXPECT_EQ(p.boxes[0], "[T----]");
  EXPECT_EQ(p.boxes[1], "[--B--]");
  ASSERT_EQ(p.boxHeights.size(), 2u);
  EXPECT_EQ(p.boxHeights[0], 5 + 3 * 16);
}

TEST(ChapterHtmlSlimParserBoxes, AHeadingsRuleMovesWithTheHeading) {
  ASSERT_GT(wordsPerLine(), 1);
  // A heading underlined by its border, at the foot of a page: it moves to keep with its text,
  // and its rule must go with it rather than stay behind under nothing.
  PageParse p(paragraph(3) + "<h2 style=\"border-bottom: 1px solid\">Head</h2>" + paragraph(3), 5);
  ASSERT_EQ(p.pages.size(), 2u);
  EXPECT_EQ(p.pages[0], "LLL");
  EXPECT_EQ(p.boxes[0], "");
  EXPECT_EQ(p.pages[1].substr(0, 2), "CL");
  EXPECT_EQ(p.boxes[1], "[--B--]");
}

// ---- Block font sizes on the built-in font ----
TEST(BuiltinFontSizes, VariantsSnapToTheBuiltInSizes) {
  EXPECT_EQ(BuiltinFontSizes::variantPt(NOTOSANS_14_FONT_ID, 1.0f), 0);   // no change
  EXPECT_EQ(BuiltinFontSizes::variantPt(NOTOSANS_14_FONT_ID, 1.03f), 0);  // too small a change
  EXPECT_EQ(BuiltinFontSizes::variantPt(NOTOSANS_14_FONT_ID, 2.0f), 18);  // capped at the top
  EXPECT_EQ(BuiltinFontSizes::variantPt(NOTOSANS_14_FONT_ID, 1.17f), 16);
  EXPECT_EQ(BuiltinFontSizes::variantPt(NOTOSANS_14_FONT_ID, 0.8f), 12);  // shrinking rounds down
  EXPECT_EQ(BuiltinFontSizes::variantPt(NOTOSANS_12_FONT_ID, 0.6f), 0);   // already the smallest
  EXPECT_EQ(BuiltinFontSizes::variantPt(NOTOSANS_18_FONT_ID, 1.5f), 0);   // already the largest
  EXPECT_EQ(BuiltinFontSizes::variantPt(0, 2.0f), 0);                     // not a built-in font
  EXPECT_EQ(BuiltinFontSizes::fontIdForPt(16), NOTOSANS_16_FONT_ID);
  EXPECT_EQ(BuiltinFontSizes::fontIdForPt(13), 0);
}

TEST(CssFontSize, RelativeSizesAndKeywordsParse) {
  EXPECT_EQ(CssParser::parseInlineStyle("font-size: 0.8em").fontSize, (CssLength{0.8f, CssUnit::Em}));
  EXPECT_EQ(CssParser::parseInlineStyle("font-size: 120%").fontSize, (CssLength{120.0f, CssUnit::Percent}));
  EXPECT_EQ(CssParser::parseInlineStyle("font-size: x-large").fontSize, (CssLength{1.5f, CssUnit::Em}));
  EXPECT_TRUE(CssParser::parseInlineStyle("font-size: 12px").hasFontSize());
  EXPECT_FALSE(CssParser::parseInlineStyle("font-size: inherit").hasFontSize());
}

TEST(ChapterHtmlSlimParserFontSizes, HeadingsGrowAndSmallPrintShrinks) {
  PageParse p("<h1>Title</h1><p>body</p><div style=\"font-size: 0.8em\"><p>note</p></div>", 12, NOTOSANS_14_FONT_ID);
  ASSERT_EQ(p.linePts.size(), 3u);
  EXPECT_EQ(p.linePts[0], 18);
  EXPECT_EQ(p.linePts[1], 0);
  EXPECT_EQ(p.linePts[2], 12);
}

TEST(ChapterHtmlSlimParserFontSizes, AbsoluteAndRootSizesLeaveTheReaderSize) {
  PageParse p("<div style=\"font-size: 30px\"><p>px</p></div>", 12, NOTOSANS_14_FONT_ID);
  ASSERT_EQ(p.linePts.size(), 1u);
  EXPECT_EQ(p.linePts[0], 0);
}

TEST(ChapterHtmlSlimParserFontSizes, BookStylesOffKeepsEveryLineAtTheReaderSize) {
  PageParse p("<h1>Title</h1><p>body</p>", 12, NOTOSANS_14_FONT_ID, /*embeddedStyle=*/false);
  ASSERT_EQ(p.linePts.size(), 2u);
  EXPECT_EQ(p.linePts[0], 0);
  EXPECT_EQ(p.linePts[1], 0);
}

TEST(ChapterHtmlSlimParserFontSizes, NestedSizesMultiply) {
  // 1.5em inside 0.67em: back to the reader size.
  PageParse p("<div style=\"font-size: 0.67em\"><div style=\"font-size: 1.5em\"><p>x</p></div></div>", 12,
              NOTOSANS_14_FONT_ID);
  ASSERT_EQ(p.linePts.size(), 1u);
  EXPECT_EQ(p.linePts[0], 0);
}

TEST(ChapterHtmlSlimParserFontSizes, ANonBuiltInFontKeepsOneSize) {
  PageParse p("<h1>Title</h1>", 12, /*fontId=*/0);
  ASSERT_EQ(p.linePts.size(), 1u);
  EXPECT_EQ(p.linePts[0], 0);
}
