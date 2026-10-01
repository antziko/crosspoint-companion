#pragma once

#include <Epub.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class ChapterXPathResolver {
 public:
  // Longest needle findTextRanges will match, in bytes. Mirrors QUOTE_PREVIEW_MAX
  // (src/BookmarkStore.h), the cap on the text a quote stores, and bounds the sliding
  // window the locator keeps over the chapter.
  static constexpr size_t kMaxNeedleBytes = 512;
  // Needles locatable in one pass. Bounds the locator's fixed result array, so the whole
  // match state stays off the heap apart from the window itself.
  static constexpr size_t kMaxTextNeedles = 8;

  /**
   * Resolve the Nth paragraph in a spine item to its real XHTML ancestry path.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]
   *
   * An empty string means parsing failed or the paragraph index was not found.
   */
  static std::string findXPathForParagraph(const std::shared_ptr<Epub>& epub, int spineIndex, uint16_t paragraphIndex);

  /**
   * Resolve several paragraphs of one spine item in a single pass over its XHTML.
   *
   * The single-paragraph overload above re-streams and re-parses the whole chapter on every
   * call, so anchoring N bookmarks in one chapter would cost N parses. This costs one.
   *
   * @param paragraphIndices `count` paragraph indices; need not be sorted, may repeat.
   * @param outXPaths        Caller-owned array of `count` strings, cleared on entry. An entry
   *                         is left empty when its paragraph was not found.
   * @return How many entries were filled.
   */
  static size_t findXPathsForParagraphs(const std::shared_ptr<Epub>& epub, int spineIndex,
                                        const uint16_t* paragraphIndices, std::string* outXPaths, size_t count);

  /**
   * Resolve several visible-codepoint offsets of one spine item in a single pass.
   *
   * Same relationship to findXPathForVisibleTextOffset as findXPathsForParagraphs has to
   * findXPathForParagraph: one chapter parse however many offsets are asked for.
   *
   * @param offsets    `count` zero-based visible-codepoint offsets; need not be sorted.
   * @param outXPaths  Caller-owned array of `count` strings, cleared on entry. An entry is
   *                   left empty when its offset fell outside paragraph/list-item text.
   * @param endOfRange Optional `count` flags. An entry marked true is the exclusive end of
   *                   a range, and resolves to an offset just past the last character of
   *                   the text node holding it rather than to the start of whatever text
   *                   follows — which is what KOReader's pos1 means, and is a different
   *                   node whenever a highlight ends at a paragraph boundary.
   * @return How many entries were filled.
   */
  static size_t findXPathsForOffsets(const std::shared_ptr<Epub>& epub, int spineIndex, const uint32_t* offsets,
                                     std::string* outXPaths, size_t count, const bool* endOfRange = nullptr);

  /** Half-open visible-codepoint range [start, end) within one spine item. */
  struct TextRange {
    uint32_t start = 0;
    uint32_t end = 0;
    bool found = false;
  };

  /**
   * Locate the visible-codepoint range of each needle in a spine item, in one pass.
   *
   * This is how a highlight gets a layout-independent anchor: CrossPoint stores a quote as
   * page-local word indices, which mean nothing once the page re-flows, but it also stores
   * the selected text. Finding that text in the chapter turns it back into a position.
   *
   * Matching is done on a canonical form of both sides — whitespace, soft hyphens and the
   * dashes a page token is split on are dropped — because the stored text is a space-joined
   * list of layout tokens while the source carries its own indentation and dash spellings.
   * A needle that does not occur, or occurs more than once, is reported not-found rather
   * than guessed at: a highlight on the wrong sentence is worse than no highlight.
   *
   * The offsets are in the same space findXPathsForOffsets consumes.
   *
   * @param needles    `count` needle strings; an empty needle is skipped.
   * @param outRanges  Caller-owned array of `count` ranges, reset on entry.
   * @return How many needles were located unambiguously.
   */
  static size_t findTextRanges(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string* needles,
                               TextRange* outRanges, size_t count);

  /**
   * Resolve a zero-based visible-codepoint offset in a spine item to its real
   * XHTML ancestry path plus text-node offset.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text()[1].0
   *
   * An empty string means parsing failed or the offset did not resolve inside
   * paragraph/list-item text.
   */
  static std::string findXPathForVisibleTextOffset(const std::shared_ptr<Epub>& epub, int spineIndex,
                                                   uint32_t visibleTextOffset);

  /**
   * Total visible codepoints in a spine item, in the space findXPathsForOffsets consumes.
   *
   * Progress is a fraction of the chapter, so turning several progress values into offsets
   * needs this once; findXPathForProgress counts it per call, which costs a chapter pass
   * per bookmark when a batch could share one.
   *
   * @return 0 when the item could not be read.
   */
  static size_t countVisibleChars(const std::shared_ptr<Epub>& epub, int spineIndex);

  /**
   * Convert intra-spine progress to an offset for findXPathsForOffsets.
   *
   * Targets are resolved inclusively, so the caller marks these entries in `endOfRange`.
   *
   * @param totalVisibleChars from countVisibleChars; 0 yields 0.
   */
  static uint32_t offsetForProgress(float intraSpineProgress, size_t totalVisibleChars);

  /**
   * Resolve intra-spine progress to a real XHTML ancestry path plus text offset.
   *
   * Returns a KOReader-compatible path like:
   * /body/DocFragment[8]/body/div[2]/section[1]/p[4]/text().96
   *
   * An empty string means parsing failed or the location could not be resolved.
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);
};
