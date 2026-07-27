#pragma once

#include <HalStorage.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
class CssParser {
 public:
  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  // v7: drop rules whose style sets no e-ink-relevant property (heap saving on CSS-heavy books)
  // v8: interpretDecoration also detects line-through (upstream #2397 text-decoration)
  static constexpr uint8_t CSS_CACHE_VERSION = 8;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found)
   */
  bool loadFromStream(HalFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(const std::string& tagName, const std::string& classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(const std::string& styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return rulesBySelector_.empty(); }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return rulesBySelector_.size(); }

  /**
   * Clear all loaded rules
   */
  void clear() {
    rulesBySelector_.clear();
    stylePool_.clear();
    loaded_ = false;
  }

  /**
   * True when a full (not heap-capped) rule set is resident. The section builder loads the
   * book's CSS once and keeps it across chapter builds instead of reloading/clearing per build
   * (which churned the heap); a heap-capped partial load stays "not fully loaded" so a later
   * build retries once the heap has recovered.
   */
  [[nodiscard]] bool isFullyLoaded() const { return loaded_ && !cssHeapBail_; }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache() const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache();

 private:
  // Distinct style values, referenced by index from rulesBySelector_. Real stylesheets have many
  // selectors that resolve to identical property sets (calibre emits hundreds of class rules
  // sharing a handful of styles), so pooling the CssStyle (104 B each) and keeping only a 2-byte
  // index per selector cuts the resident CSS heap ~2-4x — the peak that starved layout on
  // CSS-heavy books. Deduplicated in-RAM only; the on-disk cache format is unchanged (no version
  // bump, no re-parse). std::deque: element references stay valid across push_back, so the
  // indices findRule() dereferences are stable, and no single large contiguous block is needed.
  std::deque<CssStyle> stylePool_;

  // Storage: normalized selector -> index into stylePool_, kept sorted by selector.
  // std::deque, NOT std::vector: a CSS-heavy EPUB's 200-256 rules made a flat vector reallocate
  // to a single large contiguous block whose 2x doubling churn collapsed the largest free block
  // for the whole reading session. A deque stores entries in small (~0.5 KB) chunks: no giant
  // block, no doubling copy. It still supports the sorted-vector algorithm — random-access
  // iterators for findRule()'s std::lower_bound binary search and the cache-load std::sort.
  std::deque<std::pair<std::string, uint16_t>> rulesBySelector_;

  // Set true when a full (non-capped) loadFromCache() completes; drives isFullyLoaded() so the
  // section builder reloads at most once per book. Reset by clear().
  bool loaded_ = false;

  // Find-or-append `style` in stylePool_, returning its index. Linear scan (the pool is small:
  // bounded by the count of DISTINCT styles). Never mutates an existing entry, so indices already
  // handed out stay valid (copy-on-write for the stream path's applyOver merges).
  uint16_t internStyle(const CssStyle& style);

  // Set when a rule insert was skipped because free heap ran genuinely low (a deque node's
  // bare-`new` would abort() under -fno-exceptions). Once set, the rest of the parse stops storing
  // rules so the book renders with partial CSS instead of crashing. Persists across the book's CSS
  // files (heap stays tight once exhausted).
  bool cssHeapBail_ = false;

  // Binary-search lookup into the sorted rules vector. Returns nullptr if absent.
  [[nodiscard]] const CssStyle* findRule(const std::string& key) const;

  std::string cachePath;

  // Internal parsing helpers
  void processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(const std::string& declBlock);
  static void parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                        std::string& propValueBuf);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(const std::string& val);
  static CssFontStyle interpretFontStyle(const std::string& val);
  static CssFontWeight interpretFontWeight(const std::string& val);
  static CssTextDecoration interpretDecoration(const std::string& val);
  static CssLength interpretLength(const std::string& val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(const std::string& val, CssLength& out);

  // String utilities
  static std::string normalized(const std::string& s);
  static void normalizedInto(const std::string& s, std::string& out);
  static std::vector<std::string> splitOnChar(const std::string& s, char delimiter);
  static std::vector<std::string> splitWhitespace(const std::string& s);
};
