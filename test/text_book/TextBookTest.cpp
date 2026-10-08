#include <expat.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "TextBook.h"

namespace {

class StringSource : public textbook::Source {
 public:
  explicit StringSource(std::string s) : data(std::move(s)) {}
  int read(uint8_t* buf, size_t size) override {
    const size_t n = std::min(size, data.size() - pos);
    memcpy(buf, data.data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
  bool rewind() override {
    pos = 0;
    return true;
  }

 private:
  std::string data;
  size_t pos = 0;
};

struct TocItem {
  uint8_t level;
  std::string title;
  uint16_t part;
  std::string anchor;
};

class CaptureSink : public textbook::Sink {
 public:
  std::vector<std::string> parts;
  std::vector<TocItem> toc;
  bool beginPart(uint16_t index) override {
    EXPECT_EQ(index, parts.size());
    parts.emplace_back();
    return true;
  }
  bool write(const char* data, size_t len) override {
    parts.back().append(data, len);
    return true;
  }
  bool endPart() override { return true; }
  void addToc(uint8_t level, const std::string& title, uint16_t part, const std::string& anchor) override {
    toc.push_back({level, title, part, anchor});
  }
};

bool wellFormed(const std::string& xml) {
  XML_Parser p = XML_ParserCreate("UTF-8");
  const bool ok = XML_Parse(p, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_OK;
  if (!ok) ADD_FAILURE() << XML_ErrorString(XML_GetErrorCode(p)) << " at line " << XML_GetCurrentLineNumber(p);
  XML_ParserFree(p);
  return ok;
}

std::string bodyOf(const std::string& part) {
  const size_t start = part.find("<body>\n");
  const size_t end = part.rfind("</body>");
  if (start == std::string::npos || end == std::string::npos) return part;
  return part.substr(start + 7, end - start - 7);
}

struct Converted {
  textbook::Result result;
  CaptureSink sink;
};

Converted md(const std::string& text, const std::string& dir = "/Books") {
  Converted c;
  StringSource src(text);
  c.result = textbook::convertMarkdown(src, c.sink, "fallback", dir);
  EXPECT_TRUE(c.result.ok);
  for (const auto& part : c.sink.parts) EXPECT_TRUE(wellFormed(part)) << part;
  return c;
}

std::string mdBody(const std::string& text) {
  auto c = md(text);
  std::string all;
  for (const auto& part : c.sink.parts) all += bodyOf(part);
  return all;
}

Converted txt(const std::string& text) {
  Converted c;
  StringSource src(text);
  c.result = textbook::convertTxt(src, c.sink, "fallback");
  EXPECT_TRUE(c.result.ok);
  for (const auto& part : c.sink.parts) EXPECT_TRUE(wellFormed(part)) << part;
  return c;
}

// ---- part names ----

TEST(TextBookPartHref, RoundTrips) {
  EXPECT_EQ(textbook::partHref(7), "p0007.xhtml");
  EXPECT_TRUE(textbook::isPartHref("p0007.xhtml"));
  EXPECT_FALSE(textbook::isPartHref("images/p0007.xhtml"));
  EXPECT_FALSE(textbook::isPartHref("cover.jpg"));
}

// ---- TXT ----

TEST(TextBookTxt, LinesBecomeParagraphsAndBlankLinesStay) {
  auto c = txt("\xEF\xBB\xBFOne\r\n\r\nTwo & <three>\n");
  ASSERT_EQ(c.sink.parts.size(), 1u);
  EXPECT_EQ(bodyOf(c.sink.parts[0]), "<p>One</p>\n<p>&#160;</p>\n<p>Two &amp; &lt;three&gt;</p>\n");
  EXPECT_EQ(c.result.title, "fallback");
  ASSERT_EQ(c.sink.toc.size(), 1u);
}

TEST(TextBookTxt, SpacesFollowUpstreamRules) {
  auto c = txt("  two\nThree   spaces");
  EXPECT_EQ(bodyOf(c.sink.parts[0]), "<p>&#160;&#160;two</p>\n<p>Three&#160;&#160; spaces</p>\n");
}

TEST(TextBookTxt, SplitsAtLineBoundaryAfterLimit) {
  std::string text;
  const std::string line(1000, 'a');
  for (int i = 0; i < 70; i++) text += line + "\n";
  auto c = txt(text);
  EXPECT_EQ(c.sink.parts.size(), 3u);
  EXPECT_EQ(c.result.parts, 3u);
  for (const auto& part : c.sink.parts) EXPECT_EQ(bodyOf(part).find("<p>a"), 0u);
}

TEST(TextBookTxt, OverlongLineStaysOneParagraph) {
  auto c = txt(std::string(40000, 'b'));
  ASSERT_EQ(c.sink.parts.size(), 1u);
  const std::string body = bodyOf(c.sink.parts[0]);
  EXPECT_EQ(body, "<p>" + std::string(40000, 'b') + "</p>\n");
}

TEST(TextBookTxt, EmptyFileGetsOnePart) {
  auto c = txt("");
  EXPECT_EQ(c.sink.parts.size(), 1u);
}

// ---- Markdown blocks ----

TEST(TextBookMd, HeadingsGetIdsAndToc) {
  auto c = md("# Hello, World!\n\ntext\n\n## Sub *part*\n\n## Sub part\n");
  const std::string all = bodyOf(c.sink.parts[0]) + bodyOf(c.sink.parts[1]) + bodyOf(c.sink.parts[2]);
  EXPECT_NE(all.find("<h1 id=\"hello-world\">Hello, World!</h1>"), std::string::npos);
  EXPECT_NE(all.find("<h2 id=\"sub-part\">Sub <i>part</i></h2>"), std::string::npos);
  EXPECT_NE(all.find("<h2 id=\"sub-part-1\">Sub part</h2>"), std::string::npos);
  EXPECT_EQ(c.result.title, "Hello, World!");
  ASSERT_EQ(c.sink.toc.size(), 3u);
  EXPECT_EQ(c.sink.toc[1].title, "Sub part");
  EXPECT_EQ(c.sink.toc[1].level, 2);
  EXPECT_EQ(c.sink.toc[1].part, 1);
}

TEST(TextBookMd, TocLevelsStartAtShallowestHeading) {
  auto c = md("## A\n\n### B\n");
  ASSERT_EQ(c.sink.toc.size(), 2u);
  EXPECT_EQ(c.sink.toc[0].level, 1);
  EXPECT_EQ(c.sink.toc[1].level, 2);
}

TEST(TextBookMd, SetextHeadingVersusThematicBreak) {
  EXPECT_EQ(mdBody("Title\n=====\n"), "<h1 id=\"title\">Title</h1>\n");
  EXPECT_EQ(mdBody("Sub\n---\n"), "<h2 id=\"sub\">Sub</h2>\n");
  EXPECT_EQ(mdBody("para\n\n---\n"), "<p>para</p>\n<hr/>\n");
  EXPECT_EQ(mdBody("* * *\n"), "<hr/>\n");
}

TEST(TextBookMd, ParagraphLinesJoinAndHardBreaks) {
  EXPECT_EQ(mdBody("one\ntwo  \nthree\\\nfour\n"), "<p>one two<br/>three<br/>four</p>\n");
}

TEST(TextBookMd, ListsNestAndContinue) {
  EXPECT_EQ(mdBody("- a\n- b\n  - c\n- d\n"),
            "<ul>\n<li>a</li>\n<li>b<ul>\n<li>c</li>\n</ul>\n</li>\n<li>d</li>\n</ul>\n");
  EXPECT_EQ(mdBody("3. x\n4. y\n"), "<ol start=\"3\">\n<li>x</li>\n<li>y</li>\n</ol>\n");
  EXPECT_EQ(mdBody("- a\n\n- b\n"), "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
  EXPECT_EQ(mdBody("- a\n+ b\n"), "<ul>\n<li>a</li>\n</ul>\n<ul>\n<li>b</li>\n</ul>\n");
}

TEST(TextBookMd, ListItemContinuationAndSecondParagraph) {
  EXPECT_EQ(mdBody("- a\nlazy\n\n  more\n"), "<ul>\n<li>a lazy<br/>more</li>\n</ul>\n");
}

TEST(TextBookMd, ParagraphEndsBeforeList) {
  EXPECT_EQ(mdBody("text\n- item\n"), "<p>text</p>\n<ul>\n<li>item</li>\n</ul>\n");
  // An ordered marker other than 1 cannot interrupt a paragraph.
  EXPECT_EQ(mdBody("year\n1999. was\n"), "<p>year 1999. was</p>\n");
}

TEST(TextBookMd, BlockquotesNestAndLazyContinue) {
  EXPECT_EQ(mdBody("> a\nb\n\nc\n"), "<blockquote>\n<p>a b</p>\n</blockquote>\n<p>c</p>\n");
  EXPECT_EQ(mdBody("> > deep\n"), "<blockquote>\n<blockquote>\n<p>deep</p>\n</blockquote>\n</blockquote>\n");
  EXPECT_EQ(mdBody("> - q\n"), "<blockquote>\n<ul>\n<li>q</li>\n</ul>\n</blockquote>\n");
  EXPECT_EQ(mdBody("- > q\n"), "<ul>\n<li><blockquote>\n<p>q</p>\n</blockquote>\n</li>\n</ul>\n");
}

TEST(TextBookMd, FencedCodeKeepsLinesAndEscapes) {
  const std::string open = "<div style=\"margin-left:1em;text-indent:0\">";
  EXPECT_EQ(mdBody("```cpp\nif (a < b)\n\n  *x*;\n```\nafter\n"),
            open + "if (a &lt; b)<br/>&#160;<br/>&#160;&#160;*x*;</div>\n<p>after</p>\n");
  // A shorter fence does not close; an unclosed fence runs to the end.
  EXPECT_EQ(mdBody("````\n```\n"), open + "```</div>\n");
}

TEST(TextBookMd, IndentedCode) {
  const std::string open = "<div style=\"margin-left:1em;text-indent:0\">";
  EXPECT_EQ(mdBody("    code\n\n    more\n"), open + "code<br/>&#160;<br/>more</div>\n");
  EXPECT_EQ(mdBody("para\n    not code\n"), "<p>para not code</p>\n");
}

TEST(TextBookMd, Tables) {
  EXPECT_EQ(mdBody("| a | b |\n|---|:-:|\n| 1 | `x|y` |\n| 2 |\n"),
            "<table>\n<tr><th>a</th><th>b</th></tr>\n<tr><td>1</td><td><code>x|y</code></td></tr>\n"
            "<tr><td>2</td><td></td></tr>\n</table>\n");
  // Header/delimiter cell count mismatch: not a table.
  EXPECT_EQ(mdBody("a | b\n---\n"), "<h2 id=\"a--b\">a | b</h2>\n");
}

TEST(TextBookMd, FrontMatterSetsTitleAndAuthor) {
  auto c = md("---\ntitle: \"My Notes\"\nauthor: Ann\n---\n# Heading\n");
  EXPECT_EQ(c.result.title, "My Notes");
  EXPECT_EQ(c.result.author, "Ann");
  EXPECT_NE(c.sink.parts[0].find("<title>My Notes</title>"), std::string::npos);
}

TEST(TextBookMd, LeadingThematicBreakIsNotFrontMatter) {
  EXPECT_EQ(mdBody("---\nJust text.\n"), "<p>Just text.</p>\n");
}

TEST(TextBookMd, RawHtmlIsEscaped) { EXPECT_EQ(mdBody("<div>x</div>\n"), "<p>&lt;div&gt;x&lt;/div&gt;</p>\n"); }

// ---- Markdown inline ----

TEST(TextBookMd, Emphasis) {
  EXPECT_EQ(mdBody("**b** *i* ***bi*** ~~s~~ __u__ _e_\n"),
            "<p><b>b</b> <i>i</i> <b><i>bi</i></b> <del>s</del> <b>u</b> <i>e</i></p>\n");
  EXPECT_EQ(mdBody("snake_case_name and 2 * 3 * 4\n"), "<p>snake_case_name and 2 * 3 * 4</p>\n");
  EXPECT_EQ(mdBody("*a **b** c*\n"), "<p><i>a <b>b</b> c</i></p>\n");
  EXPECT_EQ(mdBody("**unclosed\n"), "<p>**unclosed</p>\n");
}

TEST(TextBookMd, CodeSpansShieldMarkup) {
  EXPECT_EQ(mdBody("`*not* <em>` and ``a ` b``\n"),
            "<p><code>*not* &lt;em&gt;</code> and <code>a ` b</code></p>\n");
}

TEST(TextBookMd, EscapesAndEntities) {
  EXPECT_EQ(mdBody("\\*lit\\* &copy; &amp; &#65; &bogus; AT&T\n"),
            "<p>*lit* \xC2\xA9 &amp; &#65; &amp;bogus; AT&amp;T</p>\n");
}

TEST(TextBookMd, ExternalLinksRenderAsText) {
  EXPECT_EQ(mdBody("[site](https://x.org \"T\") <https://y.org>\n"), "<p>site https://y.org</p>\n");
}

TEST(TextBookMd, InternalLinksResolveAcrossParts) {
  auto c = md("# One\n\nsee [two](#two) and [Two](#Two)\n\n# Two\n\nback to [one](#one), [none](#missing)\n");
  ASSERT_EQ(c.sink.parts.size(), 2u);
  EXPECT_NE(c.sink.parts[0].find("<a href=\"p0001.xhtml#two\">two</a>"), std::string::npos);
  EXPECT_NE(c.sink.parts[0].find("<a href=\"p0001.xhtml#two\">Two</a>"), std::string::npos);
  EXPECT_NE(c.sink.parts[1].find("<a href=\"p0000.xhtml#one\">one</a>"), std::string::npos);
  EXPECT_NE(c.sink.parts[1].find(", none</p>"), std::string::npos);
}

TEST(TextBookMd, ReferenceLinks) {
  EXPECT_EQ(mdBody("[a][ref], [Ref][], [ref]\n\n[ref]: #top\n\n# Top\n"),
            "<p><a href=\"p0001.xhtml#top\">a</a>, <a href=\"p0001.xhtml#top\">Ref</a>, "
            "<a href=\"p0001.xhtml#top\">ref</a></p>\n<h1 id=\"top\">Top</h1>\n");
  EXPECT_EQ(mdBody("[unknown] stays\n"), "<p>[unknown] stays</p>\n");
}

TEST(TextBookMd, Footnotes) {
  auto c = md("# A\n\nClaim[^1].\n\n# B\n\n[^1]: The note.\n");
  EXPECT_NE(c.sink.parts[0].find("Claim<sup><a href=\"p0001.xhtml#fn-1\">1</a></sup>."), std::string::npos);
  EXPECT_NE(c.sink.parts[1].find("<p id=\"fn-1\"><b>1.</b> The note.</p>"), std::string::npos);
}

TEST(TextBookMd, ImagesResolveAgainstTheBookFolder) {
  auto c = md("![A \"cat\"](img/cat%20one.png) ![up](../x.jpg) ![web](https://h/i.png)\n", "/Books/notes");
  EXPECT_EQ(bodyOf(c.sink.parts[0]),
            "<p><img src=\"Books/notes/img/cat one.png\" alt=\"A &quot;cat&quot;\"/> "
            "<img src=\"Books/x.jpg\" alt=\"up\"/> web</p>\n");
}

TEST(TextBookMd, LinkedImage) {
  EXPECT_EQ(mdBody("[![i](a.png)](#top)\n\n# Top\n"),
            "<p><a href=\"p0001.xhtml#top\"><img src=\"Books/a.png\" alt=\"i\"/></a></p>\n<h1 id=\"top\">Top</h1>\n");
}

// ---- Markdown parts ----

TEST(TextBookMd, LargeDocumentSplitsAtTopLevelOnly) {
  std::string text = "- item\n";
  const std::string para(1000, 'w');
  for (int i = 0; i < 40; i++) text += "  " + para + "\n";  // one huge list item: no split inside
  text += "\nafter\n\n";
  for (int i = 0; i < 40; i++) text += para + "\n\n";
  auto c = md(text);
  // The ~40 KB list stays whole in part 0 (no split inside it); the paragraphs after it split
  // at paragraph boundaries.
  ASSERT_EQ(c.sink.parts.size(), 3u);
  EXPECT_NE(c.sink.parts[0].find("</ul>\n<p>after</p>"), std::string::npos);
  EXPECT_EQ(bodyOf(c.sink.parts[1]).find("<p>w"), 0u);
}

TEST(TextBookMd, EmptyDocumentGetsOnePart) {
  auto c = md("");
  EXPECT_EQ(c.sink.parts.size(), 1u);
  EXPECT_EQ(c.result.title, "fallback");
}

}  // namespace
