#include <Epub/Page.h>
#include <GfxRenderer.h>
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

}  // namespace
