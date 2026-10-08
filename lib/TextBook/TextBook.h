#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Converts plain-text and Markdown books into a run of small XHTML "parts" that the EPUB
// pipeline reads like spine items. Pure logic (no Arduino/HAL) so host tests can drive it.
namespace textbook {

// Bumped whenever the generated XHTML changes, so cached parts are rebuilt.
constexpr uint8_t CONVERTER_VERSION = 1;

// A part closes at the first block boundary after this many source bytes, so even a
// multi-megabyte TXT never becomes one giant section.
constexpr uint32_t PART_SPLIT_BYTES = 32 * 1024;

class Source {
 public:
  virtual ~Source() = default;
  // Returns bytes read, 0 at end, <0 on error.
  virtual int read(uint8_t* buf, size_t size) = 0;
  virtual bool rewind() = 0;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool beginPart(uint16_t index) = 0;
  virtual bool write(const char* data, size_t len) = 0;
  virtual bool endPart() = 0;
  // level is 1-based; anchor is empty for the start of a part.
  virtual void addToc(uint8_t level, const std::string& title, uint16_t part, const std::string& anchor) = 0;
};

struct Result {
  bool ok = false;
  uint16_t parts = 0;
  std::string title;
  std::string author;
};

// "p0001.xhtml" for part 1.
std::string partHref(uint16_t index);
bool isPartHref(std::string_view href);

// fallbackTitle is used when the book names none (the filename without extension).
Result convertTxt(Source& src, Sink& sink, std::string_view fallbackTitle);

// docDir is the SD folder holding the .md file ("/Books/notes"); relative image paths
// resolve against it and are emitted root-relative without the leading slash.
Result convertMarkdown(Source& src, Sink& sink, std::string_view fallbackTitle, std::string_view docDir);

}  // namespace textbook
