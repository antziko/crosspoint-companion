#pragma once

#include <memory>
#include <new>
#include <string>
#include <string_view>

#include "TextBook.h"

namespace textbook::detail {

// Lines longer than this come back in pieces (`partial` set on all but the last).
constexpr size_t MAX_LINE_BYTES = 16 * 1024;

// Buffered line splitter over a Source. Strips a leading UTF-8 BOM and '\r'.
class LineReader {
 public:
  static constexpr size_t BUF_SIZE = 4096;

  explicit LineReader(Source& src) : src(src), buf(new (std::nothrow) uint8_t[BUF_SIZE]) {}
  bool ok() const { return buf != nullptr; }
  bool failed() const { return readError; }

  // Returns false at end of input. `bytes` is the source length consumed, newline included.
  bool next(std::string& line, bool& partial, size_t& bytes);

 private:
  bool fill();

  Source& src;
  std::unique_ptr<uint8_t[]> buf;
  size_t len = 0;
  size_t pos = 0;
  bool eof = false;
  bool readError = false;
  bool started = false;
};

void appendEscaped(std::string& out, std::string_view text);
void appendEscapedAttr(std::string& out, std::string_view text);

// Plain-text line rules shared by TXT and code blocks: leading spaces and runs of spaces
// become no-break spaces so indentation survives; control bytes become spaces.
struct SpaceState {
  bool atLineStart = true;
  size_t pendingSpaces = 0;
};
void appendPlainText(std::string& out, std::string_view text, SpaceState& st);
void finishPlainLine(std::string& out, SpaceState& st);

void appendPartHeader(std::string& out, std::string_view title);
void appendPartFooter(std::string& out);

}  // namespace textbook::detail
