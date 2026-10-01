#pragma once
#include <cstring>
#include <string>

/**
 * Parsing of the "/body/DocFragment[N]" prefix shared by every KOReader XPath.
 *
 * crengine creates one DocFragment per spine <itemref>, numbered from 1, so
 * DocFragment[N] names spine item N-1. When a book has only ONE spine item crengine
 * omits the index entirely and writes "/body/DocFragment/body/..." — the indexed form
 * does not resolve in such a book, and an indexless one does not resolve in any other.
 * Both forms must therefore be produced and accepted.
 *
 * Pure string logic (inline, no Arduino/HAL deps) so host tests can use it, mirroring
 * KOReaderDocumentId's normalization helpers.
 */
namespace DocFragmentPath {

/**
 * Build the "/body/DocFragment[N]/body" prefix an xpath for `spineIndex` must open with.
 *
 * The counterpart to find(): every generated xpath and every parsed one must agree on
 * whether the index is present, so both halves of the rule live here.
 *
 * @param spineIndex 0-based spine item index.
 * @param singleSpine True when the book has exactly one spine item, in which case the
 *                    index is omitted (`spineIndex` is then necessarily 0 and unused).
 */
inline std::string body(const int spineIndex, const bool singleSpine) {
  if (singleSpine) {
    return "/body/DocFragment/body";
  }
  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
}

/**
 * Locate the "/body/DocFragment[N]" prefix, accepting the indexless single-spine form.
 *
 * @param xpath    The XPath to inspect.
 * @param outIndex 1-based fragment index; 1 for the indexless form. Untouched on failure.
 * @param outEnd   Offset just past the prefix — where the "/body..." remainder begins.
 *                 Untouched on failure.
 * @return false when the xpath carries no DocFragment prefix, or its index is malformed.
 */
inline bool find(const std::string& xpath, int& outIndex, size_t& outEnd) {
  static constexpr char kDocFragment[] = "/body/DocFragment";
  const size_t fragPos = xpath.find(kDocFragment);
  if (fragPos == std::string::npos) {
    return false;
  }
  size_t end = fragPos + strlen(kDocFragment);

  int index = 1;
  if (end < xpath.size() && xpath[end] == '[') {
    const size_t close = xpath.find(']', end);
    if (close == std::string::npos || close == end + 1) {
      return false;
    }
    int val = 0;
    for (size_t i = end + 1; i < close; i++) {
      if (xpath[i] < '0' || xpath[i] > '9') return false;
      val = val * 10 + (xpath[i] - '0');
    }
    if (val <= 0) {
      return false;
    }
    index = val;
    end = close + 1;
  }

  outIndex = index;
  outEnd = end;
  return true;
}

/** 1-based DocFragment index, or -1 when the xpath carries none. */
inline int index(const std::string& xpath) {
  int parsed = 0;
  size_t end = 0;
  return find(xpath, parsed, end) ? parsed : -1;
}

/**
 * Does `xpath` continue with `literal` at `pos`? Bounds-checked: std::string::compare
 * with an out-of-range pos throws, which aborts under -fno-exceptions.
 */
inline bool hasAt(const std::string& xpath, const size_t pos, const char* literal) {
  const size_t len = strlen(literal);
  return pos + len <= xpath.size() && xpath.compare(pos, len, literal) == 0;
}

/** 1-based N from a trailing `text()[N]`; 1 when absent or malformed. */
inline int textNodeIndex(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()[");
  if (textPos == std::string::npos) return 1;
  const size_t numStart = textPos + 7;  // strlen("text()[")
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return 1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val > 0 ? val : 1;
}

/** 1-based N from the `[N]` of the element named by `xpath[from, to)`; 1 when absent. */
inline int elementIndex(const std::string& xpath, const size_t from, const size_t to) {
  const size_t open = xpath.rfind('[', to);
  if (open == std::string::npos || open < from) return 1;
  const size_t close = xpath.find(']', open);
  if (close == std::string::npos || close > to || close == open + 1) return 1;
  int val = 0;
  for (size_t i = open + 1; i < close; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val > 0 ? val : 1;
}

/**
 * Does `xpath` name the very start of its chapter, so that the visible-text offset is 0
 * without streaming the spine item to confirm it?
 *
 * True only for the bare fragment, its bare `/body`, or a single element directly under
 * `/body` at character 0 -- `h1/text().0`, `h2[1]/text().0`. Such a path can only be the
 * heading a chapter opens with.
 *
 * The element's own index is decisive and the reason this is not a shape test alone:
 * `h2[3]` is the THIRD h2 of the chapter, which in a long chapter is thousands of
 * characters in. Treating it as offset 0 files every mark and every reading position at
 * that heading on page 0 of the chapter.
 */
inline bool isChapterStart(const std::string& xpath) {
  if (xpath.find("/p[") != std::string::npos || xpath.find("/li[") != std::string::npos) {
    return false;
  }

  int docFragIndex = 0;
  size_t docFragEnd = 0;
  if (!find(xpath, docFragIndex, docFragEnd)) {
    return false;
  }
  if (docFragEnd == xpath.size()) {
    return true;
  }
  if (xpath[docFragEnd] == '.') {
    if (docFragEnd + 1 >= xpath.size()) {
      return false;
    }
    for (size_t i = docFragEnd + 1; i < xpath.size(); i++) {
      if (xpath[i] != '0') return false;
    }
    return true;
  }

  if (!hasAt(xpath, docFragEnd, "/body")) {
    return false;
  }
  size_t bodyContentStart = docFragEnd + strlen("/body");
  if (bodyContentStart == xpath.size()) {
    return true;
  }
  if (xpath[bodyContentStart] != '/') {
    return false;
  }
  bodyContentStart++;
  if (bodyContentStart == xpath.size()) {
    return true;
  }

  const size_t dotPos = xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos <= bodyContentStart || dotPos + 1 >= xpath.size()) {
    return false;
  }
  size_t terminalEnd = dotPos;
  static constexpr char kTextNode[] = "/text()";
  const size_t textNodePos = xpath.rfind(kTextNode, dotPos);
  if (textNodePos != std::string::npos && textNodePos >= bodyContentStart) {
    terminalEnd = textNodePos;
  }
  if (xpath.find('/', bodyContentStart) < terminalEnd) {
    return false;
  }
  // The second heading of a chapter is not its start, however the path is shaped.
  if (elementIndex(xpath, bodyContentStart, terminalEnd) > 1) {
    return false;
  }

  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] != '0') return false;
  }
  return textNodeIndex(xpath) <= 1;
}

}  // namespace DocFragmentPath
