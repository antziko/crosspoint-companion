#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ReaderRenderSpec.h"
#include "WordStore.h"
#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  // Word text lives in wordStore (chunked bump arena, NUL-terminated entries); words holds
  // 8-byte handles into it. This replaces a std::deque<std::string>: per-word string
  // objects, their SSO spills, and every hyphenation/NFC temporary were the layout path's
  // dominant small-allocation churn, and any failed implicit allocation abort()s under
  // -fno-exceptions. On arena OOM the word is dropped and hadDroppedWords() latches so the
  // section build fails readably instead of committing a cache with holes in the text.
  //
  // Handles stay in a std::deque, and rubyTexts stays a deque of strings, for the original
  // reason: a paragraph can hold thousands of tokens (CJK splits every character), and a
  // vector grows by reallocating its whole element array into one contiguous block. On the
  // ESP32-C3 that single large request fails under a fragmented heap and the throwing
  // operator new abort()s the firmware (fresh-open CJK crash). A deque grows in fixed
  // ~512 B nodes, so the largest contiguous alloc stays small regardless of token count.
  // The parallel arrays below stay vectors: 1 byte / 1 bit each, they never approach the
  // contiguous-block ceiling.
  WordStore wordStore;
  std::deque<WordStore::StoredWord> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Boundary flags use all four combinations:
  //   continues=false, noSpace=false: ordinary breakable word gap
  //   continues=false, noSpace=true:  breakable zero-width, stretchable CJK/Korean gap
  //   continues=true,  noSpace=false: unbreakable attachment
  //   continues=true,  noSpace=true:  breakable zero-width, non-stretching attachment
  std::vector<bool> wordContinues;
  std::vector<bool> wordNoSpaceBefore;
  // Focus Reading emphasis: bytes [0, wordFocusBoundary) render bold, the rest at wordStyles.
  // 0 = none. An annotation rather than a token split, so the hyphenator and line breaker still
  // see whole words; TextBlock stores emphasis the same way, so extractLine passes it through.
  std::vector<uint8_t> wordFocusBoundary;
  // Internal-link identity through tokenization, hyphenation and BiDi reorder.
  // Zero means plain text; non-zero indexes linkTargets. Kept at one byte per
  // token and discarded after layout, never added to the page-cache TextBlock.
  std::vector<uint8_t> wordLinkIds;
  std::vector<std::string> linkTargets;
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks do not carry any of this metadata.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  std::deque<std::string> rubyTexts;  // per-word ruby annotation; empty when no <ruby> in this block
  BlockStyle blockStyle;
  // Reader word spacing as a percent of the font's natural space advance. Character spacing
  // lives on blockStyle instead, because it has to survive into the serialized block.
  uint8_t wordSpacingPercent = 100;
  uint8_t paragraphIndent = PARAGRAPH_INDENT_BOOK;  // stamped like wordSpacingPercent
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  bool droppedWords = false;
  // A soft flush already emitted this paragraph's first line: the rest continues it, unindented.
  bool firstLineConsumed = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<uint16_t> visualOrderScratch;

  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  // NUL-terminated view of a stored word. Valid until that word is released.
  std::string_view wordAt(const size_t i) const { return wordStore.view(words[i]); }
  bool storeWord(std::string_view text, WordStore::StoredWord& out);
  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  // Releases and erases the first `consumed` words from every per-word array.
  void dropPrefix(size_t consumed);
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0, uint8_t linkId = 0);
  uint8_t addLinkTarget(const char* href);
  bool linkTargetMatches(uint8_t linkId, const char* href) const;
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  // Moves the first `count` words into a new block with the same style, so a block can be laid
  // out as separate paragraphs. Null (and this block untouched) on OOM or a count that would leave
  // either side empty.
  std::unique_ptr<ParsedText> takePrefix(size_t count);
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // True once any word or line was dropped because an allocation failed. Callers must treat
  // the block as incomplete and fail the section build rather than cache text with holes.
  bool hadDroppedWords() const { return droppedWords; }
  bool firstLineEmitted() const { return firstLineConsumed; }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true, int8_t characterSpacing = 0, uint8_t wordSpacingPercent = 100,
                             uint8_t paragraphIndent = PARAGRAPH_INDENT_BOOK);
};
