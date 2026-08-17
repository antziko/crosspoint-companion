#pragma once

#include <DictHtmlRenderer.h>
#include <EpdFontFamily.h>

#include <cstdint>
#include <string>
#include <vector>

#include "IpaUtils.h"  // IpaTextSpan (Wrapper scratch member)

// Pure, renderer-independent layout of dictionary HTML spans into wrapped
// display lines. Width measurement is injected via Measurer, so this module has
// no dependency on GfxRenderer / CrossPointSettings / font IDs. That decoupling
// is what lets the wrap/pagination logic be unit-tested host-side with a
// deterministic fake measurer (Tier-A litmus), while the device supplies a
// real font-metric-backed measurer.
namespace DictLayout {

// A single styled run within a display line.
struct LayoutSegment {
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool isIpa = false;  // true → render with the IPA font
};

// One wrapped display line, containing one or more styled segments.
struct LayoutLine {
  std::vector<LayoutSegment> segments;
  uint8_t indentLevel = 0;
  bool isListItem = false;
};

// Width measurement injection. fn returns the pixel width of `text` rendered at
// `style`, using the IPA font when isIpa is true and the body font otherwise.
// Function-pointer + ctx (NOT std::function) per the project's binary-size rules.
struct Measurer {
  void* ctx = nullptr;
  int (*fn)(void* ctx, const char* text, EpdFontFamily::Style style, bool isIpa) = nullptr;
  int operator()(const char* text, EpdFontFamily::Style style, bool isIpa) const { return fn(ctx, text, style, isIpa); }
};

// Pixel metrics for one wrap pass. maxWidth is the usable line width; indentStep
// is one indent level in px; bulletWidth is the list-item bullet prefix width.
struct WrapMetrics {
  int maxWidth = 0;
  int indentStep = 0;
  int bulletWidth = 0;
};

// Receives each completed display line as it is produced, in order. The callee
// may keep or discard each line (e.g. retain only one page's worth while counting
// the rest) — wrapSpans always produces every line exactly once. Function-pointer
// + ctx (NOT std::function) per the project's binary-size rules.
struct LineSink {
  void* ctx = nullptr;
  void (*fn)(void* ctx, LayoutLine&& line) = nullptr;
  void operator()(LayoutLine&& line) const { fn(ctx, std::move(line)); }
};

// Stateful word-wrapper. Spans are fed one at a time via onSpan() (so the source
// — the HTML renderer — can stream them and never materialize the whole
// definition), and each completed line is emitted to the LineSink as it is
// finished. finish() flushes the trailing line. Equivalent to the one-shot
// wrapSpans(); that overload is a thin loop over this class.
class Wrapper {
 public:
  Wrapper(const WrapMetrics& metrics, const Measurer& measure, const LineSink& sink);

  void onSpan(const StyledSpan& span);  // process one span; emit completed lines to the sink
  void finish();                        // flush the trailing in-progress line

  // True once an allocation failed. The wrap stops at that point rather than continuing with
  // dropped text — silently losing a segment would change the line breaks and render a page
  // that looks complete but is not. Callers report truncation from this
  // (DictionaryDefinitionActivity::loadPage, DictGloss::fit).
  //
  // Why this exists at all: with -fno-exceptions a failed std::string/std::vector growth calls
  // abort(), and this wrap runs on whatever heap is left after the definition's glyph prewarm
  // — measured at 2432 bytes free / 1652 largest on an X3 opening a 234-codepoint CJK entry,
  // which rebooted the device inside splitIpaRuns().
  bool oom() const { return oom_; }

 private:
  int getMixedWidth(const char* text, EpdFontFamily::Style style);
  void flushLine();
  void startLine(uint8_t indent, bool listItem);
  void appendToLine(const char* text, size_t len, EpdFontFamily::Style style, bool isIpa, int width);
  void appendMixed(const char* text, EpdFontFamily::Style style);
  void breakToken(const char* tok, EpdFontFamily::Style style, uint8_t indentLevel);

  const int maxWidth_;
  const int indentStep_;
  const int bulletWidth_;
  Measurer measure_;
  LineSink sink_;
  LayoutLine currentLine_;
  int currentX_ = 0;
  bool oom_ = false;
  std::vector<IpaTextSpan> ipaRuns_;  // reused scratch for IPA run splitting
  // Reused scratch for the word-wrap token and the codepoint-break accumulator. Members rather
  // than locals so their capacity survives across tokens and across spans: constructing a
  // std::string per token (and, in breakToken, per codepoint) was one malloc/free pair per
  // character on CJK text, which has no spaces and therefore always reaches breakToken.
  std::string tok_;
  std::string pending_;
};

// Streaming layout: word-wrap every span and emit each completed line to `sink`
// as soon as it is finished. Lets the caller hold only a subset of lines (one
// page) rather than the whole definition — this is the Stage 2a RAM fix.
void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               const LineSink& sink);

// Reference full-layout path: word-wrap every span into `out` (cleared first).
// Produces the entire definition's lines in one pass — the differential oracle
// the streaming path is checked against. Implemented on top of the sink variant.
void wrapSpans(const std::vector<StyledSpan>& spans, const WrapMetrics& metrics, const Measurer& measure,
               std::vector<LayoutLine>& out);

// Number of pages for a given line count and page size. Always >= 1.
inline int paginate(int lineCount, int linesPerPage) {
  if (linesPerPage < 1) linesPerPage = 1;
  const int pages = (lineCount + linesPerPage - 1) / linesPerPage;
  return pages < 1 ? 1 : pages;
}

}  // namespace DictLayout
