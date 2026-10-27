#pragma once

#include <HalStorage.h>

#include <deque>
#include <memory>
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
  // v9: !important is stripped from every declaration value, not only display/direction, so a
  //     declaration written "margin: 0 !important" is no longer discarded (upstream #3221)
  // v10: list-style-type is parsed and serialized (defined bit 18), so the style wire record
  //      grew by one byte (upstream #3500)
  // v11: font-weight 600 is bold (the rule cache stores the interpreted weight)
  static constexpr uint8_t CSS_CACHE_VERSION = 11;

  // The tag names and class tokens one chapter uses, so loadFromCache() keeps only the rules
  // resolveStyle() can match there. Hashes are case-insensitive FNV-1a; a class hashes with its
  // leading '.', exactly as it appears in a ".class" selector. Fixed capacity, heap-allocated once.
  class SelectorSet {
   public:
    static constexpr uint16_t CAPACITY = 512;
    // Collects from the chapter's XHTML. False (set unusable: load every rule) on OOM, a read
    // error, or more than CAPACITY distinct names.
    bool scan(HalFile& html);
    // Whether a stored selector key ("tag", ".class" or "tag.class") can match in this chapter.
    [[nodiscard]] bool wants(const std::string& key) const;
    [[nodiscard]] uint16_t size() const { return count_; }

   private:
    bool insert(uint32_t hash);
    [[nodiscard]] bool contains(uint32_t hash) const;
    std::unique_ptr<uint32_t[]> hashes_;  // sorted, unique
    uint16_t count_ = 0;
  };

  // Vertical-text interface, named as in matcha-reader so its VerticalSection compiles unchanged.
  // Our cache does not record which stylesheet a rule came from, so the filter admits everything.
  struct StylesheetFilter {
    [[nodiscard]] bool allows(uint8_t) const { return true; }
  };
  // Distilled per-block layout parameters the VERTICAL engine consumes, in em units.
  struct VerticalBlockStyle {
    float startEm = 0;         // margin-top: every column of the block starts this many cells down
    float beforeEm = 0;        // margin-right: extra gap before the block's first column
    float afterEm = 0;         // margin-left: extra gap after the block's last column
    float hangEm = 0;          // padding-top + negative text-indent: hanging indent for wrapped lines
    bool alignCenter = false;  // text-align: center -> column content vertically centered
    uint8_t borderEdges = 0;   // border edge mask; our cache records no borders, so always 0
    [[nodiscard]] bool any() const {
      return startEm > 0 || beforeEm > 0 || afterEm > 0 || hangEm > 0 || alignCenter || borderEdges != 0;
    }
  };
  // One styled-block rule keyed by hashes: the vertical engine matches "tag", ".class" and
  // "tag.class" only. tagHash 0 = any tag, classHash 0 = no class.
  struct VerticalBlockRule {
    uint32_t tagHash = 0;
    uint32_t classHash = 0;
    VerticalBlockStyle style;
  };
  // Case-insensitive FNV-1a over a selector piece (tag name or class token). Never 0.
  static uint32_t blockSelectorHash(const char* s, size_t len);
  // Stream the on-disk rules cache and collect a VerticalBlockRule for every simple selector with
  // a vertical-relevant property, without materializing the rule map. Returns the count (<= maxOut).
  size_t collectVerticalStyles(std::vector<VerticalBlockRule>& out, size_t maxOut = 256,
                               const StylesheetFilter* stylesheets = nullptr) const;

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
    // Reset the bail latch with the store it guards. Without this a low-heap bail on one section
    // permanently silenced processRuleBlockWithStyle(), so every later inline <style> block was
    // discarded too -- loadFromCache() calls clear() at entry, so the latch outlived its cause.
    cssHeapBail_ = false;
    styleGateBail_ = false;
  }

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
   * @param only If set, rules that cannot match any element in it are skipped.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache(const SelectorSet* only = nullptr);

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

  // Find-or-append `style` in stylePool_, returning its index. Linear scan (the pool is small:
  // bounded by the count of DISTINCT styles). Never mutates an existing entry, so indices already
  // handed out stay valid (copy-on-write for the stream path's applyOver merges).
  uint16_t internStyle(const CssStyle& style);

  // Set when a rule insert was skipped because free heap ran genuinely low (a deque node's
  // bare-`new` would abort() under -fno-exceptions). Once set, the rest of the parse stops storing
  // rules so the book renders with partial CSS instead of crashing. Persists across the book's CSS
  // files (heap stays tight once exhausted).
  bool cssHeapBail_ = false;

  // Set the first time resolveStyle() refused to resolve because free heap was under
  // MIN_FREE_HEAP_FOR_CSS. Latched per load (cleared in clear()) purely so the log line fires
  // once per chapter build rather than once per element -- see resolveStyle().
  mutable bool styleGateBail_ = false;

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
