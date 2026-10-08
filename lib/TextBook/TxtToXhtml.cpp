#include "TextBookInternal.h"

namespace textbook {

using namespace detail;

// One <p> per source line, so a TXT laid out one paragraph per line reads as paragraphs and
// a hard-wrapped one keeps its lines. Blank lines stay visible, as in the old TXT reader.
Result convertTxt(Source& src, Sink& sink, const std::string_view fallbackTitle) {
  Result result;
  result.title = std::string(fallbackTitle);

  LineReader reader(src);
  if (!reader.ok()) return result;

  std::string line;
  line.reserve(256);
  std::string out;
  out.reserve(1024);

  uint16_t part = 0;
  bool partOpen = false;
  uint32_t partBytes = 0;
  bool inLine = false;  // a <p> opened by a partial (over-long) line is still open
  SpaceState st;

  auto flush = [&]() {
    const bool ok = out.empty() || sink.write(out.data(), out.size());
    out.clear();
    return ok;
  };
  auto openPart = [&]() {
    if (!sink.beginPart(part)) return false;
    appendPartHeader(out, result.title);
    partOpen = true;
    partBytes = 0;
    return true;
  };
  auto closePart = [&]() {
    appendPartFooter(out);
    const bool ok = flush() && sink.endPart();
    partOpen = false;
    part++;
    return ok;
  };

  bool partial = false;
  size_t bytes = 0;
  while (reader.next(line, partial, bytes)) {
    if (!partOpen && !openPart()) return result;
    partBytes += bytes;
    if (!inLine) {
      if (line.empty() && !partial) {
        out += "<p>&#160;</p>\n";
      } else {
        out += "<p>";
        inLine = true;
      }
    }
    if (inLine) {
      appendPlainText(out, line, st);
      if (!partial) {
        out += "</p>\n";
        finishPlainLine(out, st);
        inLine = false;
      }
    }
    if (out.size() >= 2048 && !flush()) return result;
    if (!inLine && partBytes >= PART_SPLIT_BYTES && !closePart()) return result;
  }
  if (reader.failed()) return result;

  if (inLine) out += "</p>\n";
  if (!partOpen && part == 0 && !openPart()) return result;  // empty file still gets one part
  if (partOpen && !closePart()) return result;

  sink.addToc(1, result.title, 0, "");
  result.parts = part;
  result.ok = true;
  return result;
}

}  // namespace textbook
