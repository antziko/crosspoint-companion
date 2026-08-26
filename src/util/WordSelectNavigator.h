#pragma once

#include <EpdFontFamily.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class GfxRenderer;
class MappedInputManager;

// Orientation-aware word-selection navigator.
// Holds a flat list of on-screen words organised into rows and tracks the
// currently highlighted word.  handleNavigation() processes directional input;
// handleMultiSelectInput() processes Confirm/Back for multi-word selection.
// The calling activity owns single-select Confirm/Back and activity-specific logic.
class WordSelectNavigator {
 public:
  // One selectable token. Kept as small as it can be made: CJK layout tokenises one
  // word per *character* (ParsedText.cpp:399-423), so a Chinese page yields ~400 of
  // these against ~60 for the same page in English, and the flat array needs all of it
  // in ONE contiguous block. At the old 32 bytes a 256-entry growth step asked for 8192
  // against a 7412-byte largest free block and abort()ed in extractWords. Every field
  // below is sized against a single page, not a book.
  struct WordInfo {
    uint16_t textOffset = 0;
    uint16_t textLen = 0;
    uint16_t lookupOffset = 0;
    uint16_t lookupLen = 0;
    int16_t screenX = 0;
    int16_t screenY = 0;
    int16_t width = 0;
    int16_t row = 0;
    // Flat indices of the hyphenated other half (EPUB only), -1 when unpaired. int16_t
    // because these index words on one page, which is in the hundreds.
    int16_t continuationIndex = -1;  // index of hyphenated second half
    int16_t continuationOf = -1;     // index of hyphenated first half
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    // Selects between the two font ids held by the navigator (see setFonts). The
    // definition view renders IPA runs in a different font from the body; word-select
    // over book text sets neither flag nor a second font, so it always resolves to the
    // reader font. Storing the flag rather than the id keeps this struct 2-byte aligned.
    bool isIpa = false;
  };

  // Words are extracted in flat order and assigned to rows in that same order
  // (organizeIntoRows), so a row's members are always the contiguous flat range
  // [firstWord, firstWord + wordCount). Holding that as two integers rather than a
  // std::vector<int> per row removes ~14 small heap blocks per CJK page, each of which
  // used to grow by doubling *interleaved with* the large word-array growth — the
  // allocator traffic that shredded the largest free block right before the abort.
  struct Row {
    int16_t yPos = 0;
    int16_t firstWord = 0;
    int16_t wordCount = 0;
  };

  // The whole point of the two structs above is their size; a stray int would undo it.
  static_assert(sizeof(WordInfo) == 22, "WordInfo must stay 22 bytes - see the comment above");
  static_assert(sizeof(Row) == 6, "Row must stay 6 bytes - see the comment above");

  // Members of row r, by position within the row. No bounds checking: callers index
  // with values derived from rowSize(r), exactly as they did with wordIndices[i].
  int rowSize(int r) const { return rows[r].wordCount; }
  bool rowEmpty(int r) const { return rows[r].wordCount == 0; }
  int wordAt(int r, int i) const { return rows[r].firstWord + i; }
  // Position of flat index `flat` within row r. Replaces the linear scans that used to
  // search wordIndices for it.
  int posInRow(int r, int flat) const { return flat - rows[r].firstWord; }
  // Top y of row r. organizeIntoRows groups words into a row with a 2 px tolerance, so two words
  // on the same row can report screenY values a pixel or two apart. Anything that positions UI
  // relative to "the row the cursor is on" must use this, or a plain left/right step inside one
  // row moves that UI. Unchecked, like the three accessors above.
  int rowY(int r) const { return rows[r].yPos; }

  // Bounding rectangle in framebuffer coordinates. Used by the differential
  // repaint path to identify which screen region to push to the panel.
  struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
  };

  // Initial vertical placement of the selection marker. Caller (EpubReader) chooses
  // the band from current-page dwell when the dwell-marker setting is enabled; defaults
  // to Middle (legacy behaviour) for every other caller.
  enum class InitialMarker { Top, Middle, Bottom };

  // Load pre-populated, pre-organised words, rows, and string pool.
  // Places the initial selection on the row given by initialMarker (Top = ~1/4 down,
  // Middle = centre, Bottom = ~3/4 down), clamped to the available rows.
  // When consumeInitialConfirm is true, the first Confirm release is ignored
  // (prevents the long-press that opened word selection from also triggering multi-select).
  void load(std::vector<WordInfo> words, std::vector<Row> rows, std::string textPool,
            bool consumeInitialConfirm = false, InitialMarker initialMarker = InitialMarker::Middle);

  // Access null-terminated display text from the pool.
  const char* getDisplay(const WordInfo& w) const { return textPool.data() + w.textOffset; }
  // Access null-terminated lookup text from the pool.
  const char* getLookup(const WordInfo& w) const { return textPool.data() + w.lookupOffset; }

  // Fonts the word list was laid out in. Only ever two across both callers — the body
  // font, and the IPA font the definition view uses for pronunciation runs — so they
  // live here once instead of costing 4 bytes in every WordInfo. Call before load();
  // ipa defaults to body, which is what word-select over book text wants.
  void setFonts(int bodyFontId, int ipaFontId = 0) {
    bodyFontId_ = bodyFontId;
    ipaFontId_ = ipaFontId != 0 ? ipaFontId : bodyFontId;
  }
  int fontIdFor(const WordInfo& w) const { return w.isIpa ? ipaFontId_ : bodyFontId_; }

  // Whether the cursor avoids DictStopwords closed-class words ("the", "a", "you"): it is
  // placed on a content word at load(), row navigation prefers one, and a left/right step
  // walks past them. On for lookup, where those words are never the target. Off for quote
  // selection, which has to reach every word of the quote verbatim. Call before load().
  void setSkipStopwords(bool enabled) { skipStopwords_ = enabled; }

  // Organise a flat word list into rows by Y coordinate (2px tolerance).
  // Sets each word's row field and populates the rows vector.
  static void organizeIntoRows(std::vector<WordInfo>& words, std::vector<Row>& rows);

  // Link the last word of each row that ends with a trailing hyphen to the
  // first word of the next row, marking them as a compound pair via
  // continuationIndex / continuationOf. Also stores a merged lookup text
  // (hyphen stripped) shared by both halves for dictionary lookup.
  // Words whose text both starts and ends with '-' (e.g. -re-) are standalone
  // affix tokens and are skipped — they are not compound-word first halves.
  static void mergeHyphenatedPairs(std::vector<WordInfo>& words, const std::vector<Row>& rows, std::string& textPool);

  // Append a null-terminated string to a text pool. Returns the offset.
  // Uses manual linear +256 growth to avoid std::string doubling.
  static uint16_t poolAppend(std::string& pool, const char* s, size_t len);

  // Process navigation input for the current screen orientation.
  // Returns true if the selection changed (caller should requestUpdate).
  // Does NOT consume Confirm or Back.
  // swapAxes trades the two button pairs' roles: the side pair walks word by word
  // and the front pair moves between rows. Passed in rather than read from settings
  // so this file stays free of CrossPointSettings (see test/dict-word-select-nav).
  bool handleNavigation(const MappedInputManager& input, const GfxRenderer& renderer, bool swapAxes = false);

  // Currently highlighted word. nullptr if the word list is empty.
  const WordInfo* getSelected() const;

  // The paired half of the selected hyphenated word (EPUB use only).
  // When on the first half returns the second half; when on the second half returns the first.
  // Returns nullptr when the selected word has no paired half.
  const WordInfo* getPairedHalf() const;

  bool isEmpty() const { return words.empty(); }

  // Flat index of the current cursor word. -1 if empty.
  int getCurrentFlatIndex() const;

  // Word at flat index idx. nullptr if out of bounds.
  const WordInfo* getWordAt(int idx) const;

  // Join display text of words in range [fromIdx, toIdx] (inclusive, either order).
  // Returns raw joined string; caller should apply Dictionary::cleanWord() if needed.
  std::string buildPhrase(int fromIdx, int toIdx) const;

  // --- Multi-select support (shared by WordSelect and Definition activities) ---

  enum class MultiSelectAction { None, Consumed, PhraseReady, ExitedMultiSelect, EnteredMultiSelect };

  bool isMultiSelecting() const { return inMultiSelectMode; }

  // Anchor (start) flat word index of the in-progress / just-confirmed multi-select range.
  // Stays valid after handleMultiSelectInput returns PhraseReady (only the mode flag is
  // cleared there), so a caller can pair it with getCurrentFlatIndex() to recover the
  // confirmed [anchor, cursor] range. -1 when no range was started.
  int getAnchorFlatIndex() const { return anchorFlatIndex; }

  // Process Confirm/Back for multi-select state machine.
  // Returns PhraseReady when a phrase range is confirmed (raw phrase in outPhrase).
  // Returns EnteredMultiSelect on long-press Confirm that enters multi-select.
  // Returns Consumed when a long-press was detected but no valid word (caller should return).
  // Returns ExitedMultiSelect on Back during multi-select.
  // Returns None when no multi-select-relevant input occurred.
  MultiSelectAction handleMultiSelectInput(const MappedInputManager& input, std::string& outPhrase,
                                           unsigned long longPressMs = 600);

  // Draw inverted highlight for selected word(s).  Uses WordInfo::fontId.
  // In multi-select: highlights the anchor..cursor range.
  // In single-select: highlights the cursor word (+ hyphenated continuation if any).
  void renderHighlight(const GfxRenderer& renderer, int lineHeight) const;

  // Compute the union of the previous and current highlight bounding rectangles,
  // padded by 2 px on every side to cover renderHighlight's fillRect border.
  // When prevWordIdx is -1 (no previous highlight), returns just the current
  // highlight's padded bounds.
  Rect computeDirtyRect(int prevWordIdx, int currWordIdx, int lineHeight) const;

  // Differential repaint: restore pixels under the previous highlight, snapshot
  // pixels under the new highlight, then draw the new highlight. The caller
  // pushes via GfxRenderer::displayBuffer (full panel) — the savings come from
  // skipping page->render, not from a smaller push. The returned dirty rect is
  // unused by current callers; only the optional's .has_value() distinguishes
  // success from fallback.
  //
  // Returns std::nullopt when the caller must fall back to a full repaint:
  //   - the new highlight is too large for HighlightSnapshot's buffer
  //   - the selection has a hyphenated continuation (multi-word fallback for now)
  //   - the navigator is in multi-select mode
  //   - the snapshot capture is rejected by the renderer (e.g. out of bounds)
  //
  // Mutates internal snapshot state, so this method is non-const.
  std::optional<Rect> renderHighlightDifferential(GfxRenderer& renderer, int lineHeight, int prevWordIdx,
                                                  int currWordIdx);

  // Pixel snapshot for one rectangular framebuffer region. The pixel buffer is
  // inline (a fixed-size array member), so the snapshot does no heap allocation
  // — but it lives wherever its enclosing WordSelectNavigator lives, which in
  // practice is on the heap inside DictionaryWordSelectActivity /
  // DictionaryDefinitionActivity. Used by renderHighlightDifferential to
  // capture pixels under a highlight before it is drawn, so a later cursor move
  // can restore them and wipe the old highlight without re-rendering the page.
  //
  // Relationship to GfxRenderer::storeBwBuffer / restoreBwBuffer: that pair
  // captures the *entire* framebuffer to a heap-allocated buffer for grayscale
  // rendering. HighlightSnapshot is region-scoped (one word's bounding rect)
  // and stays out of the heap, so it can run on every cursor move without
  // fragmentation pressure.
  //
  // Safety: the renderer byte-aligns the captured rectangle along the
  // panel-memory x-axis, expanding the captured region by up to 7 pixels per
  // side along that axis. In Landscape orientations this is the screen
  // horizontal axis (extra pixels on the left/right of the highlight); in
  // Portrait orientations the memory x-axis is the screen vertical axis, so
  // the extra pixels sit above and below the highlight in screen space.
  // restore() pastes those back unchanged, which is correct only as long as
  // nothing else writes to the framebuffer between capture() and restore().
  // In word-select that holds: the page is drawn once on entry and the only
  // other framebuffer writer is the highlight itself. Adding a path that
  // draws over those neighboring pixels (e.g. an overlay layered above this
  // one) would invalidate that assumption.
  //
  // Sizing: 4096 bytes covers a worst-case ~800 x 40 px region (e.g., a single
  // wide word's bounding rect in landscape). When the requested region exceeds
  // capacity, capture() returns false and the caller falls back to a full
  // repaint instead.
  struct HighlightSnapshot {
    static constexpr size_t MAX_SNAPSHOT_BYTES = 4096;

    // Capture the framebuffer rectangle into the internal buffer.
    // Returns true on success; false if the region exceeds capacity, is empty,
    // is out of bounds, or the renderer rejects it.
    bool capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const GfxRenderer& renderer);

    // Paste the captured pixels back into the framebuffer at the original
    // coordinates. No-op if the snapshot is not valid.
    void restore(GfxRenderer& renderer) const;

    bool valid() const { return bytes_ > 0; }
    void clear() { bytes_ = 0; }

    uint16_t x() const { return x_; }
    uint16_t y() const { return y_; }
    uint16_t w() const { return w_; }
    uint16_t h() const { return h_; }

   private:
    uint16_t x_ = 0;
    uint16_t y_ = 0;
    uint16_t w_ = 0;
    uint16_t h_ = 0;
    size_t bytes_ = 0;
    uint8_t buf_[MAX_SNAPSHOT_BYTES] = {};
  };

  void reset();

 private:
  std::vector<WordInfo> words;
  std::vector<Row> rows;
  std::string textPool;
  int bodyFontId_ = 0;
  int ipaFontId_ = 0;
  bool skipStopwords_ = true;
  int currentRow = 0;
  int currentWordInRow = 0;
  bool inMultiSelectMode = false;
  bool confirmReleaseConsumed = false;
  int anchorFlatIndex = -1;

  // Centre X of one word — the coordinate every row-navigation comparison is made against.
  int wordCenterX(int flatIdx) const { return words[flatIdx].screenX + words[flatIdx].width / 2; }

  // Position within targetRow of the word nearest refCenterX. With preferContentWord the
  // search is run over non-stopwords first and only falls back to the whole row when it
  // holds nothing else, so row navigation stops on "harbour" rather than the "the" beside
  // it.
  int findClosestWordFromX(int targetRow, int refCenterX, bool preferContentWord = true) const;

  // One left/right step: move by a single word, wrapping at the page ends, then apply
  // hyphenated-pair smoothing so both halves of a word broken across rows count as one
  // stop. Split out of handleNavigation so the stopword skip can repeat a whole step,
  // smoothing included, rather than re-implement it.
  void advanceHorizontal(bool forward);

  // True when the word's lookup text is a DictStopwords closed-class word.
  bool isStopwordAt(int flatIdx) const;

  // Position within `row` of the first non-stopword at or beside index `pos`. Returns pos
  // unchanged when it is already a content word, or when the row holds nothing but
  // stopwords.
  int contentWordNear(int row, int pos) const;

  // Centre X of the page's whole text block, or -1 when there are no words. Seeds
  // rowNavGoalX so the first row move has a column to aim at that does not depend on which
  // word the starting row happens to hold.
  int textBlockCenterX() const;

  // Goal column for vertical navigation, in screen X. Seeded by load() from the page's text
  // block, held across row moves, and cleared by any left/right step, so a row carrying a
  // single short word (a heading, a paragraph's last line) cannot rewrite the reference to
  // its own X and drag every later row move to the start of the line -- not even when it is
  // the row the cursor started on. -1 means "derive it from the cursor", which is the state
  // a left/right step leaves behind.
  int rowNavGoalX = -1;

  // Flat index of the second half we snapped from on wordPrev. Allows subsequent
  // rowPrev/rowNext to reference that half's position rather than the first half's.
  // -1 means inactive.
  int pendingSnapIdx = -1;

  // Snapshot of pixels under the most recently drawn highlight. Used by
  // renderHighlightDifferential to restore the framebuffer before drawing the
  // next highlight, so a cursor move repaints only the affected regions.
  HighlightSnapshot snapshot_;

  // Single-word highlight draw. Used by both renderHighlight (for each word it
  // chooses to highlight) and renderHighlightDifferential.
  void drawSingleHighlight(const GfxRenderer& renderer, int lineHeight, int wordIndex) const;

  // Draw the hyphenated continuation partner(s) of w when they fall outside [lo, hi].
  // No-op when w is nullptr or w has no continuation links.
  void drawContinuationsIfOutside(const GfxRenderer& renderer, int lineHeight, const WordInfo* w, int lo, int hi) const;

  // Padded bounding rectangle for one word, matching renderHighlight's ±2 padding.
  // Returns Rect{0,0,0,0} when wordIndex is invalid.
  Rect boundsForWord(int wordIndex, int lineHeight) const;
};
