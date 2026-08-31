#include "CssParser.h"

#include <Arduino.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    }
  }

  void clear() { len = 0; }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }

  // Convert to string for passing to functions (single allocation)
  std::string str() const { return std::string(data, len); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Largest-free-block floor (bytes) required before storing another rule into the deque. A deque
// insert allocates at most one small chunk plus the entry's string copy, so this is generous
// headroom; below it, parsing stops gracefully instead of aborting on a failed `new` (X3, fragmented
// heap). See processRuleBlockWithStyle. (Was a vector-growth pre-check; the deque removed the large
// contiguous reallocation that used to be the fragmentation source.)
constexpr size_t CSS_GROWTH_HEAP_MARGIN = 6 * 1024;

// Minimum free heap required to apply CSS during rendering.
// resolveStyle() is allocation-light: CssStyle is a pure POD (enums + CssLength),
// so applyOver() and every copy touch zero heap. The only transient allocations are
// a couple of short (SSO-eligible) std::strings plus a small splitWhitespace() vector
// when the element carries a class attribute -- well under 1KB in practice. This gate
// exists solely to skip that work before a genuine OOM-abort of those small strings
// (bare `new` aborts under -fno-exceptions), NOT as a general low-heap proxy.
//
// Sized at 4KB: still 4x the real transient, and deliberately far below where a chapter build
// actually runs. Every byte above the true requirement is harm, because tripping this gate does
// not degrade gracefully -- resolveStyle() returns an EMPTY style for every element, the chapter
// lays out completely unstyled, and Section then CACHES that layout, so one transient dip
// unstyles the chapter permanently. The previous 16KB assumed "mid-build free heap sits
// ~41-48KB"; an X3 with SD fonts installed sits far lower (observed: 62KB at BUILD-START, minus
// the build's own ~28KB reserve, minus the SD-font mini-bitmap caches loading mid-build =
// 10,960 free), so the gate tripped on ordinary books and every one of them rendered with no
// CSS at all.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 4 * 1024;

// Heap the rule store may consume while loadFromCache() populates it. A CSS-heavy book's stylesheet
// is 200+ large CssStyle entries (~60KB), and loadFromCache() runs INSIDE a section build
// (Section::startBuild -> getCssParser()->loadFromCache()), AFTER the build's pre-parse floor gate.
// Without a cap the full stylesheet loads, consumes the heap the layout needs, and the section builds
// 0 pages -- the reader then shows "Out of bounds" (no page to display).
//
// This is measured as ACTUAL consumption (free heap at entry minus free heap now), not as an absolute
// floor the heap must stay above. The absolute-floor version of this gate was the bug: it reserved a
// fixed 24KB largest contiguous block, which a fragmented X3 heap never has, so the check tripped
// before rule 0 and every book silently rendered with ZERO stylesheet rules ("cache-load reserve
// bail: rules=0/52 free=62388 largest=17396" -- free was never the problem). Those same builds then
// completed fine with 156/33/39 pages, so the reserve bought nothing and cost the whole stylesheet.
// Budgeting consumption bounds the harm CSS can do to the layout that follows -- which is all the
// reserve was ever trying to do -- while being immune to whatever fragmentation already existed.
// 12KB: a normal 52-rule stylesheet costs ~7KB (~130B/rule, see processRuleBlockWithStyle), so it
// loads complete; the pathological 200+-rule case still gets capped. Rules past the budget are
// dropped (partial CSS -- still readable). Applied only on the cache path, which runs during builds
// -- NOT the stream path, which can run standalone during initial metadata caching and must be free
// to store the full stylesheet into the cache.
constexpr size_t CSS_CACHE_BYTE_BUDGET = 12 * 1024;

// Rules always loaded before the budget above is allowed to stop the load. Guarantees that a small
// stylesheet is never truncated to nothing by a transient heap dip mid-load; the hard largest-block
// floor (CSS_GROWTH_HEAP_MARGIN) still applies from rule 0, so this cannot drive an OOM abort.
constexpr size_t CSS_MIN_RULES = 16;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (value.substr(suffixPos) != IMPORTANT) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

}  // anonymous namespace

// String utilities implementation

std::string CssParser::normalized(const std::string& s) {
  std::string result;
  result.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  // Remove trailing space
  while (!result.empty() && (result.back() == ' ' || result.back() == '\n')) {
    result.pop_back();
  }
  return result;
}

void CssParser::normalizedInto(const std::string& s, std::string& out) {
  out.clear();
  out.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

std::vector<std::string> CssParser::splitOnChar(const std::string& s, const char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;

  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == delimiter) {
      std::string part = s.substr(start, i - start);
      std::string trimmed = normalized(part);
      if (!trimmed.empty()) {
        parts.push_back(trimmed);
      }
      start = i + 1;
    }
  }
  return parts;
}

std::vector<std::string> CssParser::splitWhitespace(const std::string& s) {
  std::vector<std::string> parts;
  size_t start = 0;
  bool inWord = false;

  for (size_t i = 0; i <= s.size(); ++i) {
    const bool isSpace = i == s.size() || isCssWhitespace(s[i]);
    if (isSpace && inWord) {
      parts.push_back(s.substr(start, i - start));
      inWord = false;
    } else if (!isSpace && !inWord) {
      start = i;
      inWord = true;
    }
  }
  return parts;
}

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "left" || v == "start") return CssTextAlign::Left;
  if (v == "right" || v == "end") return CssTextAlign::Right;
  if (v == "center") return CssTextAlign::Center;
  if (v == "justify") return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(const std::string& val) {
  const std::string v = normalized(val);

  if (v == "italic" || v == "oblique") return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(const std::string& val) {
  const std::string v = normalized(val);

  // Named values
  if (v == "bold" || v == "bolder") return CssFontWeight::Bold;
  if (v == "normal" || v == "lighter") return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  char* endPtr = nullptr;
  const long numericWeight = std::strtol(v.c_str(), &endPtr, 10);

  // If we parsed a number and consumed the whole string
  if (endPtr != v.c_str() && *endPtr == '\0') {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }

  return CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(const std::string& val) {
  const std::string v = normalized(val);

  // text-decoration can have multiple space-separated values. Detect underline and
  // line-through independently (upstream #2397 added line-through); "none" wins nothing
  // to combine, so an absent keyword simply leaves that bit clear.
  CssTextDecoration result = CssTextDecoration::None;
  if (v.find("underline") != std::string::npos) {
    result = result | CssTextDecoration::Underline;
  }
  if (v.find("line-through") != std::string::npos) {
    result = result | CssTextDecoration::LineThrough;
  }
  return result;
}

CssLength CssParser::interpretLength(const std::string& val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(const std::string& val, CssLength& out) {
  const std::string v = normalized(val);
  if (v.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  const std::string numPart = v.substr(0, unitStart);
  const std::string unitPart = v.substr(unitStart);

  char* endPtr = nullptr;
  const float numericValue = std::strtof(numPart.c_str(), &endPtr);
  if (endPtr == numPart.c_str()) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  auto unit = CssUnit::Pixels;
  if (unitPart == "em") {
    unit = CssUnit::Em;
  } else if (unitPart == "rem") {
    unit = CssUnit::Rem;
  } else if (unitPart == "pt") {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                          std::string& propValueBuf) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string::npos || colonPos == 0) return;

  normalizedInto(decl.substr(0, colonPos), propNameBuf);
  normalizedInto(decl.substr(colonPos + 1), propValueBuf);

  if (propNameBuf.empty() || propValueBuf.empty()) return;

  // Strip !important from EVERY value, not only display/direction as before. A declaration
  // written "margin-top: 0 !important" otherwise reached tryInterpretLength() with the
  // suffix attached, failed to parse, and left the margin at its inherited value -- which is
  // how container spacing survived with extra paragraph spacing turned off (#3221).
  const std::string_view stripped = stripTrailingImportant(propValueBuf);
  if (stripped.size() != propValueBuf.size()) propValueBuf.resize(stripped.size());
  if (propValueBuf.empty()) return;

  if (propNameBuf == "text-align") {
    style.textAlign = interpretAlignment(propValueBuf);
    style.defined.textAlign = 1;
  } else if (propNameBuf == "font-style") {
    style.fontStyle = interpretFontStyle(propValueBuf);
    style.defined.fontStyle = 1;
  } else if (propNameBuf == "font-weight") {
    style.fontWeight = interpretFontWeight(propValueBuf);
    style.defined.fontWeight = 1;
  } else if (propNameBuf == "text-decoration" || propNameBuf == "text-decoration-line") {
    style.textDecoration = interpretDecoration(propValueBuf);
    style.defined.textDecoration = 1;
  } else if (propNameBuf == "text-indent") {
    style.textIndent = interpretLength(propValueBuf);
    style.defined.textIndent = 1;
  } else if (propNameBuf == "margin-top") {
    style.marginTop = interpretLength(propValueBuf);
    style.defined.marginTop = 1;
  } else if (propNameBuf == "margin-bottom") {
    style.marginBottom = interpretLength(propValueBuf);
    style.defined.marginBottom = 1;
  } else if (propNameBuf == "margin-left") {
    style.marginLeft = interpretLength(propValueBuf);
    style.defined.marginLeft = 1;
  } else if (propNameBuf == "margin-right") {
    style.marginRight = interpretLength(propValueBuf);
    style.defined.marginRight = 1;
  } else if (propNameBuf == "margin") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.marginTop = interpretLength(values[0]);
      style.marginRight = values.size() >= 2 ? interpretLength(values[1]) : style.marginTop;
      style.marginBottom = values.size() >= 3 ? interpretLength(values[2]) : style.marginTop;
      style.marginLeft = values.size() >= 4 ? interpretLength(values[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (propNameBuf == "padding-top") {
    style.paddingTop = interpretLength(propValueBuf);
    style.defined.paddingTop = 1;
  } else if (propNameBuf == "padding-bottom") {
    style.paddingBottom = interpretLength(propValueBuf);
    style.defined.paddingBottom = 1;
  } else if (propNameBuf == "padding-left") {
    style.paddingLeft = interpretLength(propValueBuf);
    style.defined.paddingLeft = 1;
  } else if (propNameBuf == "padding-right") {
    style.paddingRight = interpretLength(propValueBuf);
    style.defined.paddingRight = 1;
  } else if (propNameBuf == "padding") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.paddingTop = interpretLength(values[0]);
      style.paddingRight = values.size() >= 2 ? interpretLength(values[1]) : style.paddingTop;
      style.paddingBottom = values.size() >= 3 ? interpretLength(values[2]) : style.paddingTop;
      style.paddingLeft = values.size() >= 4 ? interpretLength(values[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (propNameBuf == "height") {
    CssLength len;
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (propNameBuf == "width") {
    CssLength len;
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (propNameBuf == "display") {
    style.display = (propValueBuf == "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (propNameBuf == "direction") {
    if (propValueBuf == "rtl") {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (propValueBuf == "ltr") {
      style.direction = CssTextDirection::Ltr;
      style.defined.direction = 1;
    }
  } else if (propNameBuf == "vertical-align") {
    const std::string v = normalized(propValueBuf);
    if (v == "super") {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (v == "sub") {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    }
  }
}

CssStyle CssParser::parseDeclarations(const std::string& declBlock) {
  CssStyle style;
  std::string propNameBuf;
  std::string propValueBuf;

  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        const size_t len = i - start;
        std::string decl = declBlock.substr(start, len);
        if (!decl.empty()) {
          parseDeclarationIntoStyle(decl, style, propNameBuf, propValueBuf);
        }
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style) {
  // A prior insert hit the heap floor (see below) — stop storing rules entirely.
  if (cssHeapBail_) {
    return;
  }

  // Skip rules that define no supported property before splitting selectors (upstream #2604).
  // Mirrors the anySet() guard at the insert site; applyOver(emptyStyle) is a no-op, so
  // bailing here changes nothing but avoids the selector-split/normalize churn.
  if (!style.defined.anySet()) {
    return;
  }

  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Handle comma-separated selectors
  const auto selectors = splitOnChar(selectorGroup, ',');

  for (const auto& sel : selectors) {
    // Validate selector length before processing
    if (sel.size() > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
      continue;
    }

    // Normalize the selector
    std::string key = normalized(sel);
    if (key.empty()) continue;

    // TODO: Consider adding support for sibling css selectors in the future
    // Ensure no + in selector as we don't support adjacent CSS selectors for now
    if (key.find('+') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for direct nested css selectors in the future
    // Ensure no > in selector as we don't support nested CSS selectors for now
    if (key.find('>') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for attribute css selectors in the future
    // Ensure no [ in selector as we don't support attribute CSS selectors for now
    if (key.find('[') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for pseudo selectors in the future
    // Ensure no : in selector as we don't support pseudo CSS selectors for now
    if (key.find(':') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for ID css selectors in the future
    // Ensure no # in selector as we don't support ID CSS selectors for now
    if (key.find('#') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for general sibling combinator selectors in the future
    // Ensure no ~ in selector as we don't support general sibling combinator CSS selectors for now
    if (key.find('~') != std::string_view::npos) {
      continue;
    }

    // TODO: Consider adding support for wildcard css selectors in the future
    // Ensure no * in selector as we don't support wildcard CSS selectors for now
    if (key.find('*') != std::string_view::npos) {
      continue;
    }

    // TODO: Add support for more complex selectors in the future
    // At the moment, we only ever check for `tag`, `tag.class1` or `.class1`
    // If the selector has whitespace in it, then it's either a CSS selector for a descendant element (e.g. `tag1 tag2`)
    // or some other slightly more advanced CSS selector which we don't support yet
    if (key.find(' ') != std::string_view::npos) {
      continue;
    }

    // Skip if this would exceed the rule limit
    if (rulesBySelector_.size() >= MAX_RULES) {
      LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
      return;
    }

    // Store or merge with existing (vector kept sorted by selector for binary search).
    // Skip rules that set no e-ink-relevant property: resolveStyle only ever
    // applyOver()s a matched rule, and applyOver(emptyStyle) is a no-op, so an
    // absent selector and a present-but-empty one produce identical output.
    // Calibre stylesheets often carry hundreds of class rules that set only
    // color / font-family / text-transform (all unsupported) — storing each
    // wasted ~130 B of heap and could push a CSS-heavy book past the parser's
    // heap floor, leaving the reader stuck on the "out of bounds" screen.
    auto it =
        std::lower_bound(rulesBySelector_.begin(), rulesBySelector_.end(), key,
                         [](const std::pair<std::string, uint16_t>& e, const std::string& k) { return e.first < k; });
    // Both branches below grow the deque (a pool entry and/or a rule node) by at most one small
    // (~0.5 KB) chunk — never the large contiguous block the old flat vector needed. Bail
    // gracefully if the largest free block runs genuinely low, so a pathological stylesheet can't
    // exhaust it (a deque node's bare `new` aborts under -fno-exceptions); the book then renders
    // with partial CSS instead of crashing. Guarding on the largest block (not total free) keeps
    // the check meaningful once the heap is fragmented.
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < CSS_GROWTH_HEAP_MARGIN) {
      SdDebugLog::log("CSS", "rule-store bail: rules=%u largest=%u free=%u", (unsigned)rulesBySelector_.size(),
                      (unsigned)largest, (unsigned)ESP.getFreeHeap());
      LOG_ERR("CSS", "Low heap, stopping CSS parse at %u rules (largest %u)", (unsigned)rulesBySelector_.size(),
              (unsigned)largest);
      cssHeapBail_ = true;
      return;
    }
    if (it != rulesBySelector_.end() && it->first == key) {
      // Merge onto the existing selector's style. Copy-on-write: never mutate the shared pool
      // entry (other selectors may reference the same index) — merge a copy and re-intern it.
      CssStyle merged = (it->second < stylePool_.size()) ? stylePool_[it->second] : CssStyle{};
      merged.applyOver(style);
      it->second = internStyle(merged);
    } else if (style.defined.anySet()) {
      rulesBySelector_.insert(it, std::make_pair(key, internStyle(style)));
    }
  }
}

// Main parsing entry point

bool CssParser::loadFromStream(HalFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;
  // Keep these as std::string since they're passed by reference to parseDeclarationIntoStyle
  std::string propNameBuf;
  std::string propValueBuf;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        if (selector.size() > MAX_SELECTOR_LENGTH * 4) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!skippingRule && !declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector.str(), currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        if (!declBuffer.empty()) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
          declBuffer.clear();
        }
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  return true;
}

// Style resolution

uint16_t CssParser::internStyle(const CssStyle& style) {
  for (size_t i = 0; i < stylePool_.size(); ++i) {
    if (stylePool_[i] == style) return static_cast<uint16_t>(i);
  }
  stylePool_.push_back(style);
  return static_cast<uint16_t>(stylePool_.size() - 1);
}

const CssStyle* CssParser::findRule(const std::string& key) const {
  const auto it =
      std::lower_bound(rulesBySelector_.begin(), rulesBySelector_.end(), key,
                       [](const std::pair<std::string, uint16_t>& e, const std::string& k) { return e.first < k; });
  if (it != rulesBySelector_.end() && it->first == key && it->second < stylePool_.size()) {
    return &stylePool_[it->second];
  }
  return nullptr;
}

CssStyle CssParser::resolveStyle(const std::string& tagName, const std::string& classAttr) const {
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_CSS) {
    // Latched per load, not per boot: this is the one bail that silently unstyles a whole
    // chapter AND caches the result, so it has to be visible on every build it happens on --
    // and on the SD log, since the X3 has no serial and LOG_DBG alone left it undiagnosable.
    if (!styleGateBail_) {
      styleGateBail_ = true;
      SdDebugLog::log("CSS", "style gate bail: free=%u < %u largest=%u - chapter lays out unstyled", freeHeap,
                      (unsigned)MIN_FREE_HEAP_FOR_CSS, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      LOG_ERR("CSS", "Low heap (%u B) below MIN_FREE_HEAP_FOR_CSS (%u) - returning empty style", freeHeap,
              static_cast<unsigned>(MIN_FREE_HEAP_FOR_CSS));
    }
    return CssStyle{};
  }
  CssStyle result;
  const std::string tag = normalized(tagName);

  // 1. Apply element-level style (lowest priority)
  if (const CssStyle* tagStyle = findRule(tag)) {
    result.applyOver(*tagStyle);
  }

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority)
  if (!classAttr.empty()) {
    const auto classes = splitWhitespace(classAttr);

    for (const auto& cls : classes) {
      std::string classKey = "." + normalized(cls);
      if (const CssStyle* classStyle = findRule(classKey)) {
        result.applyOver(*classStyle);
      }
    }

    // TODO: Support combinations of classes (e.g. style on p.class1.class2)
    // 3. Apply element.class styles (higher priority)
    for (const auto& cls : classes) {
      std::string combinedKey = tag + "." + normalized(cls);
      if (const CssStyle* combinedStyle = findRule(combinedKey)) {
        result.applyOver(*combinedStyle);
      }
    }
  }

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string& styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Write version
  file.write(CssParser::CSS_CACHE_VERSION);

  // Write rule count
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));

  // Write each rule: selector string + CssStyle fields
  for (const auto& pair : rulesBySelector_) {
    if (pair.second >= stylePool_.size()) continue;  // defensive: skip a dangling index
    // Write selector string (length-prefixed)
    const auto selectorLen = static_cast<uint16_t>(pair.first.size());
    file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    file.write(reinterpret_cast<const uint8_t*>(pair.first.data()), selectorLen);

    // Write CssStyle fields (all are POD types). Styles are pooled in RAM (dedup); the on-disk
    // format stays flat — one full style per selector — so the cache version is unchanged.
    const CssStyle& style = stylePool_[pair.second];
    file.write(static_cast<uint8_t>(style.textAlign));
    file.write(static_cast<uint8_t>(style.fontStyle));
    file.write(static_cast<uint8_t>(style.fontWeight));
    file.write(static_cast<uint8_t>(style.textDecoration));
    file.write(static_cast<uint8_t>(style.direction));

    // Write CssLength fields (value + unit)
    auto writeLength = [&file](const CssLength& len) {
      file.write(reinterpret_cast<const uint8_t*>(&len.value), sizeof(len.value));
      file.write(static_cast<uint8_t>(len.unit));
    };

    writeLength(style.textIndent);
    writeLength(style.marginTop);
    writeLength(style.marginBottom);
    writeLength(style.marginLeft);
    writeLength(style.marginRight);
    writeLength(style.paddingTop);
    writeLength(style.paddingBottom);
    writeLength(style.paddingLeft);
    writeLength(style.paddingRight);
    writeLength(style.imageHeight);
    writeLength(style.imageWidth);
    file.write(static_cast<uint8_t>(style.display));
    file.write(static_cast<uint8_t>(style.verticalAlign));

    // Write defined flags as uint32_t
    uint32_t definedBits = 0;
    if (style.defined.textAlign) definedBits |= 1 << 0;
    if (style.defined.fontStyle) definedBits |= 1 << 1;
    if (style.defined.fontWeight) definedBits |= 1 << 2;
    if (style.defined.textDecoration) definedBits |= 1 << 3;
    if (style.defined.textIndent) definedBits |= 1 << 4;
    if (style.defined.marginTop) definedBits |= 1 << 5;
    if (style.defined.marginBottom) definedBits |= 1 << 6;
    if (style.defined.marginLeft) definedBits |= 1 << 7;
    if (style.defined.marginRight) definedBits |= 1 << 8;
    if (style.defined.paddingTop) definedBits |= 1 << 9;
    if (style.defined.paddingBottom) definedBits |= 1 << 10;
    if (style.defined.paddingLeft) definedBits |= 1 << 11;
    if (style.defined.paddingRight) definedBits |= 1 << 12;
    if (style.defined.imageHeight) definedBits |= 1 << 13;
    if (style.defined.imageWidth) definedBits |= 1 << 14;
    if (style.defined.display) definedBits |= 1 << 15;
    if (style.defined.direction) definedBits |= 1 << 16;
    if (style.defined.verticalAlign) definedBits |= 1 << 17;
    file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // Clear existing rules
  clear();

  // Read and verify version
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return false;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    rulesBySelector_.clear();
    return false;
  }

  // rulesBySelector_ is a std::deque (chunked storage); no reserve() and, deliberately, no single
  // contiguous block for all rules — that block was the heap-fragmentation source this container
  // change fixes. emplace_back below grows it one small chunk at a time.

  auto hasRemainingBytes = [&file](const size_t neededBytes) -> bool {
    return static_cast<size_t>(file.available()) >= neededBytes;
  };

  constexpr size_t CSS_LENGTH_FIELD_COUNT = 11;
  constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
  constexpr size_t CSS_FIXED_STYLE_BYTES =
      5 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) + sizeof(uint8_t) + sizeof(uint32_t);

  // Free heap when the load started; the budget below is measured against it (see
  // CSS_CACHE_BYTE_BUDGET). Sampled after clear() so the outgoing stylesheet's memory is already
  // back in the pool and does not count as this load's consumption.
  const uint32_t freeAtStart = ESP.getFreeHeap();

  // Read each rule
  for (uint16_t i = 0; i < ruleCount; ++i) {
    // Two independent stop conditions. Bail keeps the rules loaded so far -- partial CSS renders
    // fine, and unlike the old absolute-reserve gate this can no longer stop at zero rules.
    const uint32_t freeNow = ESP.getFreeHeap();
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t consumed = (freeAtStart > freeNow) ? (freeAtStart - freeNow) : 0;

    // 1. Hard safety floor, from rule 0. Purely to stop before a deque node's bare `new` aborts
    //    under -fno-exceptions -- not a layout reserve. Same floor the stream path uses.
    // 2. Consumption budget, once the minimum tranche is in.
    const bool hardFloor = largest < CSS_GROWTH_HEAP_MARGIN;
    const bool overBudget = i >= CSS_MIN_RULES && consumed >= CSS_CACHE_BYTE_BUDGET;
    if (hardFloor || overBudget) {
      SdDebugLog::log("CSS", "cache-load bail (%s): rules=%u/%u consumed=%u/%u free=%u largest=%u",
                      hardFloor ? "floor" : "budget", (unsigned)rulesBySelector_.size(), (unsigned)ruleCount,
                      (unsigned)consumed, (unsigned)CSS_CACHE_BYTE_BUDGET, (unsigned)freeNow, (unsigned)largest);
      LOG_ERR("CSS", "Stopping cache load at %u/%u rules (%s, consumed %u B)", (unsigned)rulesBySelector_.size(),
              (unsigned)ruleCount, hardFloor ? "heap floor" : "budget", (unsigned)consumed);
      cssHeapBail_ = true;
      break;
    }

    // Read selector string
    uint16_t selectorLen = 0;
    if (!hasRemainingBytes(sizeof(selectorLen))) {
      rulesBySelector_.clear();
      return false;
    }
    if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      rulesBySelector_.clear();
      return false;
    }

    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH || !hasRemainingBytes(selectorLen)) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      rulesBySelector_.clear();
      return false;
    }

    std::string selector;
    selector.resize(selectorLen);
    if (file.read(&selector[0], selectorLen) != selectorLen) {
      rulesBySelector_.clear();
      return false;
    }

    if (!hasRemainingBytes(CSS_FIXED_STYLE_BYTES)) {
      LOG_DBG("CSS", "Truncated CSS cache while reading style payload");
      rulesBySelector_.clear();
      return false;
    }

    // Read CssStyle fields
    CssStyle style;
    uint8_t enumVal;

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textAlign = static_cast<CssTextAlign>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontStyle = static_cast<CssFontStyle>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.fontWeight = static_cast<CssFontWeight>(enumVal);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.textDecoration = static_cast<CssTextDecoration>(enumVal & CSS_TEXT_DECORATION_MASK);

    if (file.read(&enumVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.direction = static_cast<CssTextDirection>(enumVal);

    // Read CssLength fields
    auto readLength = [&file](CssLength& len) -> bool {
      if (file.read(&len.value, sizeof(len.value)) != sizeof(len.value)) {
        return false;
      }
      uint8_t unitVal;
      if (file.read(&unitVal, 1) != 1) {
        return false;
      }
      len.unit = static_cast<CssUnit>(unitVal);
      return true;
    };

    if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
        !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
        !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
        !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
      rulesBySelector_.clear();
      return false;
    }

    // Read display value
    uint8_t displayVal;
    if (file.read(&displayVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.display = static_cast<CssDisplay>(displayVal);

    // Read verticalAlign value
    uint8_t verticalAlignVal;
    if (file.read(&verticalAlignVal, 1) != 1) {
      rulesBySelector_.clear();
      return false;
    }
    style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlignVal);

    // Read defined flags
    uint32_t definedBits = 0;
    if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) {
      rulesBySelector_.clear();
      return false;
    }
    style.defined.textAlign = (definedBits & 1 << 0) != 0;
    style.defined.fontStyle = (definedBits & 1 << 1) != 0;
    style.defined.fontWeight = (definedBits & 1 << 2) != 0;
    style.defined.textDecoration = (definedBits & 1 << 3) != 0;
    style.defined.textIndent = (definedBits & 1 << 4) != 0;
    style.defined.marginTop = (definedBits & 1 << 5) != 0;
    style.defined.marginBottom = (definedBits & 1 << 6) != 0;
    style.defined.marginLeft = (definedBits & 1 << 7) != 0;
    style.defined.marginRight = (definedBits & 1 << 8) != 0;
    style.defined.paddingTop = (definedBits & 1 << 9) != 0;
    style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
    style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
    style.defined.paddingRight = (definedBits & 1 << 12) != 0;
    style.defined.imageHeight = (definedBits & 1 << 13) != 0;
    style.defined.imageWidth = (definedBits & 1 << 14) != 0;
    style.defined.display = (definedBits & 1 << 15) != 0;
    style.defined.direction = (definedBits & 1 << 16) != 0;
    style.defined.verticalAlign = (definedBits & 1 << 17) != 0;

    // Defend against caches that still carry empty rules (see store-time note):
    // an empty style contributes nothing to resolveStyle, so don't hold its heap.
    // internStyle() deduplicates identical styles into the shared pool (the heap saving).
    if (style.defined.anySet()) {
      rulesBySelector_.emplace_back(std::move(selector), internStyle(style));
    }
  }

  // Cache entries are written in (unordered) container order; restore the sorted
  // invariant that findRule()'s binary search relies on.
  std::sort(rulesBySelector_.begin(), rulesBySelector_.end(),
            [](const std::pair<std::string, uint16_t>& a, const std::pair<std::string, uint16_t>& b) {
              return a.first < b.first;
            });

  LOG_DBG("CSS", "Loaded %u/%u rules from cache%s (%u distinct styles)", (unsigned)rulesBySelector_.size(), ruleCount,
          cssHeapBail_ ? " (heap-capped)" : "", (unsigned)stylePool_.size());
  return true;
}
