#include "ChapterXPathResolver.h"

#include <Epub/VisibleTextUtils.h>
#include <Epub/htmlEntities.h>
#include <Logging.h>
#include <Memory.h>
#include <Print.h>
#include <SdDebugLog.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "DocFragmentPath.h"

namespace {
std::string stripPrefix(const XML_Char* name) {
  if (!name) {
    return "";
  }

  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

struct NameCounter {
  std::string name;
  int count;
};

struct ParentState {
  std::vector<NameCounter> children;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }

    children.push_back({name, 1});
    return 1;
  }
};

struct PathSegment {
  std::string name;
  int index;
};

std::string buildParagraphXPath(const int spineIndex, const bool singleSpine, const std::vector<PathSegment>& path,
                                const int textNodeIndex, const size_t charOffset) {
  std::string xpath = DocFragmentPath::body(spineIndex, singleSpine);
  for (const auto& segment : path) {
    xpath += "/" + segment.name + "[" + std::to_string(segment.index) + "]";
  }
  if (textNodeIndex > 0) {
    xpath += "/text()[" + std::to_string(textNodeIndex) + "]." + std::to_string(charOffset);
  }
  return xpath;
}

size_t countUtf8Codepoints(const XML_Char* data, const int len) {
  if (!data || len <= 0) {
    return 0;
  }

  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }

  return count;
}

// Named HTML entities (&nbsp;, &ldquo;, ...) are not XML. Left to expat, one in a chapter with
// no DOCTYPE is a parse error that ends the pass, and one under an XHTML DOCTYPE is silently
// dropped -- while the page parser expands both (ChapterHtmlSlimParser::defaultHandlerExpand).
// Every pass here expands them the same way, so all of them count the text the page shows.
template <void (*CharacterData)(void*, const XML_Char*, int)>
void XMLCALL expandHtmlEntity(void* userData, const XML_Char* s, const int len) {
  if (len < 3 || s[0] != '&' || s[len - 1] != ';') return;
  const char* value = lookupHtmlEntity(s, static_cast<size_t>(len));
  if (value) {
    CharacterData(userData, value, static_cast<int>(strlen(value)));
  } else {
    CharacterData(userData, s, len);  // unknown: kept verbatim, as the page parser does
  }
}

void XMLCALL ignoreText(void*, const XML_Char*, int) {}

class ParagraphTextCounter final : public Print {
 public:
  ParagraphTextCounter() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ParagraphTextCounter::startElement, &ParagraphTextCounter::endElement);
    XML_SetCharacterDataHandler(parser, &ParagraphTextCounter::characterData);
    XML_SetDefaultHandlerExpand(parser, &expandHtmlEntity<&ParagraphTextCounter::characterData>);
  }

  ~ParagraphTextCounter() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  size_t totalVisibleChars() const { return visibleChars; }

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<ParagraphTextCounter*>(userData);
    self->onCharacterData(data, len);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }
    if (name == "p") {
      paragraphDepth++;
    }
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || paragraphDepth <= 0 || len <= 0) {
      return;
    }

    visibleChars += countUtf8Codepoints(data, len);
  }

 private:
  XML_Parser parser = nullptr;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  uint16_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
};

// Resolves any number of paragraph indices in ONE pass over the spine item. Resolving an
// xpath costs a full re-stream of the chapter's XHTML, so a caller anchoring several
// bookmarks in the same chapter pays for one parse rather than one per bookmark.
// `targets` and `outputs` are caller-owned parallel arrays of `count` entries; they need
// not be sorted, and duplicate targets each get filled.
class XPathParagraphResolver final : public Print {
 public:
  XPathParagraphResolver(const uint16_t* targets, std::string* outputs, const size_t count)
      : targets(targets), outputs(outputs), count(count), remaining(count) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathParagraphResolver::startElement, &XPathParagraphResolver::endElement);
    // Counts no text, but an entity must not end the pass with a parse error.
    XML_SetDefaultHandlerExpand(parser, &expandHtmlEntity<&ignoreText>);
  }

  ~XPathParagraphResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  // How many of the requested paragraphs were found.
  size_t matchCount() const { return count - remaining; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;
  bool singleSpine = false;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathParagraphResolver*>(userData);
    self->onEndElement(name);
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();

    // Count both <p> and <li> as paragraph-like positions, matching how the section
    // layout tracks them (xpathParagraphIndex and xpathListItemIndex). This ensures
    // KOReader progress in list items maps to the correct XPath.
    //
    // Matching happens only here, inside the increment: testing on every element would
    // let a 0 target bind to whatever element precedes the first paragraph.
    if (name == "p" || name == "li") {
      paragraphCount++;
      // Linear scan of the (small, unsorted) target set: cheap next to XML parsing, and it
      // spares the caller from sorting and mapping indices back. A non-empty slot means
      // "already filled" — safe because a built path always carries the DocFragment prefix
      // and so is never empty.
      for (size_t i = 0; i < count; i++) {
        if (!outputs[i].empty() || targets[i] != paragraphCount) continue;
        outputs[i] = buildParagraphXPath(spineIndex, singleSpine, path, 0, 0);
        remaining--;
      }
      if (remaining == 0) {
        stopped = true;
        XML_StopParser(parser, XML_FALSE);
      }
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      return;
    }

    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  XML_Parser parser = nullptr;
  const uint16_t* targets;
  std::string* outputs;
  const size_t count;
  size_t remaining;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphCount = 0;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
};

// Resolves any number of visible-codepoint offsets in ONE pass over the spine item, the
// same economy XPathParagraphResolver gives paragraph indices. `targets` and `outputs` are
// caller-owned parallel arrays of `count` entries; they need not be sorted.
class XPathProgressResolver final : public Print {
 public:
  // Exclusive places offset N on the character at N. Inclusive places it just past the
  // character at N-1, which keeps a range end inside the text node the range ran through
  // instead of moving it to the start of the next one.
  enum class BoundaryMode { Exclusive, Inclusive };

  XPathProgressResolver(const uint32_t* targets, std::string* outputs, const size_t count,
                        const BoundaryMode boundaryMode = BoundaryMode::Exclusive,
                        const bool* perTargetInclusive = nullptr)
      : targets(targets),
        outputs(outputs),
        count(count),
        remaining(count),
        boundaryMode(boundaryMode),
        perTargetInclusive(perTargetInclusive) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &XPathProgressResolver::startElement, &XPathProgressResolver::endElement);
    XML_SetCharacterDataHandler(parser, &XPathProgressResolver::characterData);
    XML_SetDefaultHandlerExpand(parser, &expandHtmlEntity<&XPathProgressResolver::characterData>);
    XML_SetCommentHandler(parser, &XPathProgressResolver::comment);
    XML_SetProcessingInstructionHandler(parser, &XPathProgressResolver::processingInstruction);
    XML_SetCdataSectionHandler(parser, &XPathProgressResolver::startCdataSection,
                               &XPathProgressResolver::endCdataSection);
  }

  ~XPathProgressResolver() override { destroyXmlParser(parser); }

  bool ok() const { return parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk || stopped) {
      return parseOk;
    }

    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
    }
    return parseOk;
  }

  size_t matchCount() const { return count - remaining; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk || stopped) {
      return size;
    }

    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        parseOk = false;
      }
    }

    return size;
  }

  int spineIndex = 0;
  bool singleSpine = false;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onCharacterData(data, len);
  }

  static void XMLCALL comment(void* userData, const XML_Char*) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL processingInstruction(void* userData, const XML_Char*, const XML_Char*) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL startCdataSection(void* userData) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  static void XMLCALL endCdataSection(void* userData) {
    auto* self = static_cast<XPathProgressResolver*>(userData);
    self->onMarkupBoundary();
  }

  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
        parentStates.emplace_back();
      }
      depth++;
      return;
    }

    const int siblingIndex = parentStates.back().nextIndex(name);
    path.push_back({name, siblingIndex});
    parentStates.emplace_back();
    textNodeIndexStack.push_back(0);
    pendingTextNode = true;

    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }

    if (name == "p") {
      paragraphDepth++;
    }
    if (name == "li") {
      liDepth++;
    }

    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);

    depth--;
    if (!insideBody) {
      return;
    }

    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      parentStates.clear();
      path.clear();
      textNodeIndexStack.clear();
      nonVisibleDepth = 0;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
    }
    if (name == "p" && paragraphDepth > 0) {
      paragraphDepth--;
    }
    if (name == "li" && liDepth > 0) {
      liDepth--;
    }

    if (!textNodeIndexStack.empty()) {
      textNodeIndexStack.pop_back();
    }
    if (paragraphDepth > 0 || liDepth > 0) {
      pendingTextNode = true;
    }
    if (!path.empty()) {
      path.pop_back();
    }
    if (!parentStates.empty()) {
      parentStates.pop_back();
    }
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || (paragraphDepth <= 0 && liDepth <= 0) || len <= 0 || stopped) {
      return;
    }

    const size_t codepointCount = countUtf8Codepoints(data, len);
    if (codepointCount == 0) {
      return;
    }

    // Start a new text node on first non-empty content after any structural boundary.
    // Only counting non-empty nodes matches KOReader's text()[N] indexing behavior,
    // which skips empty text nodes created by bare <a id="anchor"/> anchors.
    if (pendingTextNode) {
      if (!textNodeIndexStack.empty()) {
        textNodeIndexStack.back()++;
      }
      textNodeStartChars = visibleChars;
      pendingTextNode = false;
    }

    const size_t nextVisibleChars = visibleChars + codepointCount;
    const int texNode = textNodeIndexStack.empty() ? 0 : textNodeIndexStack.back();
    for (size_t i = 0; i < count; i++) {
      // A built path is never empty (it always carries the DocFragment prefix), so an
      // empty output is an unambiguous "still pending" and `remaining` cannot underflow.
      if (!outputs[i].empty()) continue;
      const size_t target = targets[i];
      if (target < visibleChars) continue;  // fell in an earlier chunk with no text node
      const bool inclusive =
          boundaryMode == BoundaryMode::Inclusive || (perTargetInclusive != nullptr && perTargetInclusive[i]);
      const bool inChunk = inclusive ? target <= nextVisibleChars : target < nextVisibleChars;
      if (!inChunk) continue;
      outputs[i] = buildParagraphXPath(spineIndex, singleSpine, path, texNode, target - textNodeStartChars);
      remaining--;
    }
    if (remaining == 0) {
      stopped = true;
      XML_StopParser(parser, XML_FALSE);
      return;
    }

    visibleChars = nextVisibleChars;
  }

  void onMarkupBoundary() {
    if (!insideBody || nonVisibleDepth > 0 || (paragraphDepth <= 0 && liDepth <= 0) || stopped || pendingTextNode) {
      return;
    }

    pendingTextNode = true;
  }

  XML_Parser parser = nullptr;
  const uint32_t* targets;
  std::string* outputs;
  const size_t count;
  size_t remaining;
  const BoundaryMode boundaryMode;
  const bool* perTargetInclusive;
  bool parseOk = true;
  bool insideBody = false;
  bool stopped = false;
  bool pendingTextNode = true;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  uint16_t nonVisibleDepth = 0;
  size_t visibleChars = 0;
  size_t textNodeStartChars = 0;
  std::vector<int> textNodeIndexStack;
  std::vector<ParentState> parentStates;
  std::vector<PathSegment> path;
};

// A quote's stored preview is the page's layout tokens joined with single spaces, while the
// source XHTML carries its own indentation, line breaks and dash spellings — "east west" on
// the page came from "east--west" in the file. Matching one against the other only works in
// a canonical form, so both sides drop every character the two representations disagree
// about: whitespace, soft hyphens, and the dashes PageTokens splits a token on.
bool isDroppedCodepoint(const uint32_t cp) {
  switch (cp) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case 0x00A0:  // no-break space
    case '-':
    case 0x00AD:  // soft hyphen
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x2013:  // en dash
    case 0x2014:  // em dash
      return true;
    default:
      return false;
  }
}

// Substring search over a byte range that is not NUL-terminated. memmem would do, but it
// is a GNU extension and the device links against newlib.
const char* findBytes(const char* hay, const size_t hayLen, const char* needle, const size_t needleLen) {
  if (needleLen == 0 || needleLen > hayLen) return nullptr;
  const char* const last = hay + (hayLen - needleLen);
  for (const char* p = hay; p <= last;) {
    const void* c = std::memchr(p, needle[0], static_cast<size_t>(last - p) + 1);
    if (!c) return nullptr;
    p = static_cast<const char*>(c);
    if (std::memcmp(p, needle, needleLen) == 0) return p;
    p++;
  }
  return nullptr;
}

std::string normalizeForMatch(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(in.data());
  const unsigned char* end = ptr + in.size();
  while (ptr < end) {
    const unsigned char* start = ptr;
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (!isDroppedCodepoint(cp)) {
      out.append(reinterpret_cast<const char*>(start), static_cast<size_t>(ptr - start));
    }
  }
  return out;
}

// Finds where each needle's text sits in a spine item, as a visible-codepoint range in the
// same space XPathProgressResolver consumes, so the two passes compose.
//
// Matching runs over a sliding window of the canonical text rather than the whole chapter:
// a needle is capped at QUOTE_PREVIEW_MAX, so a window of that plus one parse chunk is
// enough to catch any match, including one straddling a chunk boundary. The window carries
// a parallel raw-offset per byte, which is what turns a byte match back into a position.
class ChapterTextLocator final : public Print {
 public:
  ChapterTextLocator(const std::string* needles, ChapterXPathResolver::TextRange* out, const size_t count,
                     const bool anyBlock)
      : outRanges(out), count(count), anyBlock(anyBlock) {
    size_t longest = 0;
    for (size_t i = 0; i < count; i++) {
      normalized[i] = normalizeForMatch(needles[i]);
      // A needle longer than the window could never be held whole and so would report a
      // silent miss; say so instead. QUOTE_PREVIEW_MAX keeps real quotes inside this.
      if (normalized[i].size() > ChapterXPathResolver::kMaxNeedleBytes) {
        LOG_DBG("KOX", "Needle of %u bytes exceeds the %u-byte match window; skipped",
                static_cast<unsigned>(normalized[i].size()),
                static_cast<unsigned>(ChapterXPathResolver::kMaxNeedleBytes));
        normalized[i].clear();
      }
      longest = std::max(longest, normalized[i].size());
      lastAbsStart[i] = SIZE_MAX;
    }
    // Retain enough of the window that a needle straddling two parse chunks still matches.
    retain = longest;
    // Fixed capacity, allocated once: the window never grows past one needle plus one parse
    // chunk, and a vector/string reserve would abort rather than fail on a full heap.
    windowCapacity = retain + kChunkBytes;
    window = makeUniqueNoThrow<char[]>(windowCapacity);
    rawAt = makeUniqueNoThrow<uint32_t[]>(windowCapacity);
    if (!window || !rawAt) {
      LOG_ERR("KOX", "OOM: %u bytes for the quote match window",
              static_cast<unsigned>(windowCapacity * (1 + sizeof(uint32_t))));
      allocOk = false;
      return;
    }

    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      LOG_ERR("KOX", "Failed to create XML parser");
      return;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &ChapterTextLocator::startElement, &ChapterTextLocator::endElement);
    XML_SetCharacterDataHandler(parser, &ChapterTextLocator::characterData);
    XML_SetDefaultHandlerExpand(parser, &expandHtmlEntity<&ChapterTextLocator::characterData>);
  }

  ~ChapterTextLocator() override { destroyXmlParser(parser); }

  bool ok() const { return allocOk && parser != nullptr && parseOk; }

  bool finish() {
    if (!parser || !parseOk) {
      return parseOk;
    }
    if (XML_Parse(parser, "", 0, XML_TRUE) == XML_STATUS_ERROR) {
      LOG_ERR("KOX", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser)));
      noteParseError();
    }
    return parseOk;
  }

  // Written only after finish(): a needle is not accepted until the whole chapter has been
  // seen, because a second occurrence anywhere in it makes the first one ambiguous.
  size_t commit() {
    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
      if (hits[i] == 1) {
        outRanges[i] = pending[i];
        outRanges[i].found = true;
        found++;
      } else if (hits[i] > 1) {
        LOG_DBG("KOX", "Quote text occurs %u times in spine %d; not anchored", static_cast<unsigned>(hits[i]),
                spineIndex);
      }
      outRanges[i].occurrences = static_cast<uint16_t>(std::min<size_t>(hits[i], UINT16_MAX));
    }
    return found;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!parser || !parseOk) {
      return size;
    }
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) {
        LOG_ERR("KOX", "XML parse error: %s", XML_ErrorString(error));
        noteParseError();
      }
    }
    return size;
  }

  int spineIndex = 0;
  // Diagnostics for a chapter that yields nothing: the first parse error and where it hit, how
  // much visible text was seen, and (anyBlock passes only) each needle's longest matched
  // opening with the chapter bytes that followed it.
  int errorCode = 0;
  unsigned long errorLine = 0;
  unsigned long errorColumn = 0;
  size_t visibleSeen() const { return visibleChars; }
  struct NearMiss {
    uint16_t best = 0;
    char got[17] = {};
  };
  NearMiss nearMiss[ChapterXPathResolver::kMaxTextNeedles];

 private:
  void noteParseError() {
    if (parseOk) {
      errorCode = static_cast<int>(XML_GetErrorCode(parser));
      errorLine = static_cast<unsigned long>(XML_GetCurrentLineNumber(parser));
      errorColumn = static_cast<unsigned long>(XML_GetCurrentColumnNumber(parser));
    }
    parseOk = false;
  }

  static constexpr size_t kChunkBytes = 1024;  // the read size findTextRanges streams with

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<ChapterTextLocator*>(userData)->onStartElement(name);
  }

  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<ChapterTextLocator*>(userData)->onEndElement(name);
  }

  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<ChapterTextLocator*>(userData)->onCharacterData(data, len);
  }

  // Visibility bookkeeping mirrors XPathProgressResolver exactly — p and li, outside any
  // non-visible element — because the offsets produced here are consumed by that resolver.
  void onStartElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        bodyDepth = depth;
      }
      depth++;
      return;
    }
    if (nonVisibleDepth > 0 || VisibleTextUtils::isNonVisibleElement(name)) {
      nonVisibleDepth++;
    }
    if (name == "p") paragraphDepth++;
    if (name == "li") liDepth++;
    depth++;
  }

  void onEndElement(const XML_Char* rawName) {
    const std::string name = stripPrefix(rawName);
    depth--;
    if (!insideBody) {
      return;
    }
    if (depth == bodyDepth && name == "body") {
      insideBody = false;
      nonVisibleDepth = 0;
      return;
    }
    if (nonVisibleDepth > 0) nonVisibleDepth--;
    if (name == "p" && paragraphDepth > 0) paragraphDepth--;
    if (name == "li" && liDepth > 0) liDepth--;
  }

  void onCharacterData(const XML_Char* data, const int len) {
    if (!insideBody || nonVisibleDepth > 0 || (!anyBlock && paragraphDepth <= 0 && liDepth <= 0) || len <= 0) {
      return;
    }

    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
    const unsigned char* end = ptr + len;
    while (ptr < end) {
      const unsigned char* start = ptr;
      const uint32_t cp = utf8NextCodepoint(&ptr);
      if (!isDroppedCodepoint(cp)) {
        for (const unsigned char* b = start; b < ptr; b++) {
          // Expat hands text over in pieces no larger than the parse buffer, so the window
          // cannot overrun between slides. Drop anything that somehow would rather than
          // write past it — a missed anchor, never a corrupted one.
          if (windowLen >= windowCapacity) {
            scanWindow();
            slideWindow();
          }
          if (windowLen >= windowCapacity) break;
          window[windowLen] = static_cast<char>(*b);
          rawAt[windowLen] = static_cast<uint32_t>(visibleChars);
          windowLen++;
        }
      }
      visibleChars++;
    }

    scanWindow();
    slideWindow();
  }

  void scanWindow() {
    if (anyBlock) trackNearMisses();
    for (size_t i = 0; i < count; i++) {
      const std::string& needle = normalized[i];
      if (needle.empty() || hits[i] > 1) continue;  // already ambiguous; no need to keep counting
      if (needle.size() > windowLen) continue;
      size_t from = 0;
      while (from + needle.size() <= windowLen) {
        const char* hit = findBytes(window.get() + from, windowLen - from, needle.data(), needle.size());
        if (!hit) break;
        const size_t at = static_cast<size_t>(hit - window.get());
        const size_t absStart = absBase + at;
        from = at + 1;
        // The retained tail is rescanned on every chunk, so skip anything already counted.
        if (lastAbsStart[i] != SIZE_MAX && absStart <= lastAbsStart[i]) continue;
        lastAbsStart[i] = absStart;
        if (++hits[i] > 1) break;
        pending[i].start = rawAt[at];
        // The needle's last byte belongs to its last codepoint; the range is half-open, so
        // the end is that codepoint's index plus one.
        pending[i].end = rawAt[at + needle.size() - 1] + 1;
      }
    }
  }

  void trackNearMisses() {
    for (size_t i = 0; i < count; i++) {
      const std::string& needle = normalized[i];
      if (needle.empty()) continue;
      NearMiss& m = nearMiss[i];
      for (size_t at = 0; at < windowLen; at++) {
        if (window[at] != needle[0]) continue;
        size_t n = 0;
        while (n < needle.size() && at + n < windowLen && window[at + n] == needle[n]) n++;
        // A run cut off by the window's end is not a divergence; the next scan sees it whole.
        if (n <= m.best || (at + n == windowLen && n < needle.size())) continue;
        m.best = static_cast<uint16_t>(n);
        const size_t take = std::min<size_t>(sizeof(m.got) - 1, windowLen - (at + n));
        memcpy(m.got, window.get() + at + n, take);
        m.got[take] = '\0';
      }
    }
  }

  void slideWindow() {
    if (windowLen <= retain) return;
    const size_t drop = windowLen - retain;
    std::memmove(window.get(), window.get() + drop, retain);
    std::memmove(rawAt.get(), rawAt.get() + drop, retain * sizeof(uint32_t));
    windowLen = retain;
    absBase += drop;
  }

  XML_Parser parser = nullptr;
  ChapterXPathResolver::TextRange* outRanges;
  const size_t count;
  const bool anyBlock;
  std::string normalized[ChapterXPathResolver::kMaxTextNeedles];
  size_t hits[ChapterXPathResolver::kMaxTextNeedles] = {};
  size_t lastAbsStart[ChapterXPathResolver::kMaxTextNeedles] = {};
  ChapterXPathResolver::TextRange pending[ChapterXPathResolver::kMaxTextNeedles] = {};
  // The canonical text seen so far, trimmed to one needle's worth after every parse chunk,
  // with the visible-codepoint index of each byte alongside it.
  std::unique_ptr<char[]> window;
  std::unique_ptr<uint32_t[]> rawAt;
  size_t windowCapacity = 0;
  size_t windowLen = 0;
  size_t retain = 0;
  size_t absBase = 0;
  size_t visibleChars = 0;
  bool allocOk = true;
  bool parseOk = true;
  bool insideBody = false;
  int depth = 0;
  int bodyDepth = -1;
  int paragraphDepth = 0;
  int liDepth = 0;
  uint16_t nonVisibleDepth = 0;
};

}  // namespace

size_t ChapterXPathResolver::findXPathsForParagraphs(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                     const uint16_t* paragraphIndices, std::string* outXPaths,
                                                     const size_t count) {
  if (!paragraphIndices || !outXPaths || count == 0) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    outXPaths[i].clear();
  }
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return 0;
  }

  // Paragraph indices are 1-based, so a 0 target can never match. Bail before streaming
  // when none of them can: parsing a whole chapter to find nothing is pure waste, and a
  // caller anchoring bookmarks may well hand us a set with no usable index in it. The
  // single-paragraph entry point guarded this before it became a wrapper.
  bool anyTarget = false;
  for (size_t i = 0; i < count && !anyTarget; i++) {
    anyTarget = paragraphIndices[i] != 0;
  }
  if (!anyTarget) {
    return 0;
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return 0;
  }

  XPathParagraphResolver resolver(paragraphIndices, outXPaths, count);
  if (!resolver.ok()) {
    return 0;
  }

  resolver.spineIndex = spineIndex;
  resolver.singleSpine = epub->getSpineItemsCount() == 1;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return 0;
  }

  const size_t found = resolver.matchCount();
  LOG_DBG("KOX", "Resolved %u/%u paragraph(s) in spine %d", static_cast<unsigned>(found), static_cast<unsigned>(count),
          spineIndex);
  return found;
}

std::string ChapterXPathResolver::findXPathForParagraph(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                        const uint16_t paragraphIndex) {
  std::string xpath;
  findXPathsForParagraphs(epub, spineIndex, &paragraphIndex, &xpath, 1);
  return xpath;
}

size_t ChapterXPathResolver::findXPathsForOffsets(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                  const uint32_t* offsets, std::string* outXPaths, const size_t count,
                                                  const bool* endOfRange) {
  if (!offsets || !outXPaths || count == 0) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    outXPaths[i].clear();
  }
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return 0;
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return 0;
  }

  XPathProgressResolver resolver(offsets, outXPaths, count, XPathProgressResolver::BoundaryMode::Exclusive, endOfRange);
  if (!resolver.ok()) {
    return 0;
  }

  resolver.spineIndex = spineIndex;
  resolver.singleSpine = epub->getSpineItemsCount() == 1;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return 0;
  }

  const size_t found = resolver.matchCount();
  LOG_DBG("KOX", "Resolved %u/%u visible offset(s) in spine %d", static_cast<unsigned>(found),
          static_cast<unsigned>(count), spineIndex);
  return found;
}

std::string ChapterXPathResolver::findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                                const uint32_t visibleTextOffset) {
  std::string xpath;
  findXPathsForOffsets(epub, spineIndex, &visibleTextOffset, &xpath, 1);
  return xpath;
}

size_t ChapterXPathResolver::countVisibleChars(const std::shared_ptr<Epub>& epub, const int spineIndex) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return 0;
  }
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return 0;
  }
  ParagraphTextCounter counter;
  if (!counter.ok() || !epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
    return 0;
  }
  return counter.totalVisibleChars();
}

uint32_t ChapterXPathResolver::offsetForProgress(const float intraSpineProgress, const size_t totalVisibleChars) {
  if (totalVisibleChars == 0) {
    return 0;
  }
  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t target =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));
  return static_cast<uint32_t>(target);
}

std::string ChapterXPathResolver::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                       const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return "";
  }

  if (!(intraSpineProgress > 0.0f)) {
    return DocFragmentPath::body(spineIndex, epub->getSpineItemsCount() == 1);
  }

  ParagraphTextCounter counter;
  if (!counter.ok() || !epub->readItemContentsToStream(href, counter, 1024) || !counter.finish()) {
    return "";
  }

  const size_t totalVisibleChars = counter.totalVisibleChars();
  if (totalVisibleChars == 0) {
    return "";
  }

  const float clamped = std::max(0.0f, std::min(1.0f, intraSpineProgress));
  const size_t targetVisibleChar =
      std::max<size_t>(1, std::min(totalVisibleChars, static_cast<size_t>(std::ceil(clamped * totalVisibleChars))));

  const uint32_t target = static_cast<uint32_t>(targetVisibleChar);
  std::string xpath;
  XPathProgressResolver resolver(&target, &xpath, 1, XPathProgressResolver::BoundaryMode::Inclusive);
  if (!resolver.ok()) {
    return "";
  }

  resolver.spineIndex = spineIndex;
  resolver.singleSpine = epub->getSpineItemsCount() == 1;
  if (!epub->readItemContentsToStream(href, resolver, 1024) || !resolver.finish()) {
    return "";
  }

  if (!xpath.empty()) {
    LOG_DBG("KOX", "Resolved progress %.3f in spine %d -> %s", intraSpineProgress, spineIndex, xpath.c_str());
    return xpath;
  }

  LOG_DBG("KOX", "Could not resolve progress %.3f in spine %d", intraSpineProgress, spineIndex);
  return "";
}

size_t ChapterXPathResolver::findTextRanges(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                            const std::string* needles, TextRange* outRanges, const size_t count,
                                            const bool anyBlock) {
  if (!needles || !outRanges || count == 0 || count > kMaxTextNeedles) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    outRanges[i] = TextRange{};
  }
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return 0;
  }

  bool anyNeedle = false;
  for (size_t i = 0; i < count && !anyNeedle; i++) {
    anyNeedle = !needles[i].empty();
  }
  if (!anyNeedle) {
    return 0;
  }

  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty()) {
    return 0;
  }

  ChapterTextLocator locator(needles, outRanges, count, anyBlock);
  if (!locator.ok()) {
    SdDebugLog::log("KOX", "spine %d: quote locator not ready (OOM or parser)", spineIndex);
    return 0;
  }
  locator.spineIndex = spineIndex;
  const bool streamed = epub->readItemContentsToStream(href, locator, 1024);
  const bool parsed = streamed && locator.finish();
  if (!parsed || anyBlock) {
    SdDebugLog::log("KOX", "spine %d: locate %s streamed=%d xmlErr=%d \"%s\" at %lu:%lu visible=%u", spineIndex,
                    anyBlock ? "anyBlock" : "para", streamed ? 1 : 0, locator.errorCode,
                    locator.errorCode ? XML_ErrorString(static_cast<XML_Error>(locator.errorCode)) : "",
                    locator.errorLine, locator.errorColumn, static_cast<unsigned>(locator.visibleSeen()));
  }
  if (anyBlock) {
    for (size_t i = 0; i < count; i++) {
      if (needles[i].empty()) continue;
      SdDebugLog::log("KOX", "spine %d: needle %u matched %u/%u bytes, then \"%s\"", spineIndex,
                      static_cast<unsigned>(i), locator.nearMiss[i].best,
                      static_cast<unsigned>(normalizeForMatch(needles[i]).size()), locator.nearMiss[i].got);
    }
  }
  if (!parsed) return 0;

  const size_t found = locator.commit();
  LOG_DBG("KOX", "Located %u/%u quote text(s) in spine %d", static_cast<unsigned>(found), static_cast<unsigned>(count),
          spineIndex);
  return found;
}
