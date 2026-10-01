#pragma once
#include <Epub/Page.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictGloss.h"
#include "util/DictionaryLookupController.h"
#include "util/PageMarks.h"
#include "util/WordSelectNavigator.h"

class DictionaryWordSelectActivity final : public Activity {
 public:
  // Dictionary: Confirm/long-press selects a word/phrase and looks it up (default).
  // HighlightRange: the same single-word + long-press-range selection gesture, but
  // Confirm emits a HighlightRangeResult (page-local [start,end] word indices + joined
  // text) for the caller to save as a quote, with no dictionary lookup.
  enum class Mode { Dictionary, HighlightRange };

  // reservedBottomHeight is the post-bezel reserved space the caller (EpubReader)
  // left below the page text — status-bar height OR auto-page-turn indicator
  // height, per the caller's own layout formula. The skip-initial-render fast
  // path clears exactly that strip so the framebuffer matches the menu→lookup
  // path (no status bar, no auto-turn label visible during word-select).
  explicit DictionaryWordSelectActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page, int marginLeft, int marginTop,
      const std::string& cachePath, const std::string& nextPageFirstWord = "", bool framebufferContainsPage = false,
      int reservedBottomHeight = 0, Mode mode = Mode::Dictionary,
      WordSelectNavigator::InitialMarker initialMarker = WordSelectNavigator::InitialMarker::Middle,
      const std::string& chapterTitle = "", int initialPointX = -1, int initialPointY = -1)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        cachePath(cachePath),
        nextPageFirstWord(nextPageFirstWord),
        controller(renderer, mappedInput, *this, cachePath),
        framebufferContainsPage_(framebufferContainsPage),
        reservedBottomHeight_(reservedBottomHeight),
        mode_(mode),
        initialMarker_(initialMarker),
        chapterTitle_(chapterTitle),
        initialPointX_(initialPointX),
        initialPointY_(initialPointY) {}

  // Gloss-box sessions start at the page's first word outside a title, or at `resumeIdx` (a flat word index
  // from an earlier session on this same page, -1 for none), and write the cursor's flat index
  // back through `out` on exit. `out` belongs to the reader, which outlives this screen.
  // The reader page's identity, so every repaint here draws the same looked-up-word underlines
  // and quote marks the reader drew.
  void setPageMarkKey(const PageMarks::PageKey& key) { markKey_ = key; }

  void setGlossCursorMemory(const int resumeIdx, int* out) {
    glossResumeIdx_ = resumeIdx;
    glossCursorOut_ = out;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Redraws the reader's page (word boxes over it), so it follows the reading
  // surface's night-mode polarity; a normal-polarity flash mid-lookup jars.
  // The manual screen refresh blanks the framebuffer from the main loop, so the snapshot and
  // dirty-rect state the differential path would restore describes pixels that are no longer
  // there. Same reset the sub-activity returns take.
  void onFramebufferInvalidated() override { forceFullRepaintOnNextRender(); }
  // An overlay on the reader's own page, walked through on the way to a definition and on the
  // way back. The entry scrub therefore skips it in both directions, which is what
  // makes a lookup flash once on the definition and once on the page it returns to, instead of
  // at every step. The gloss box keeps its own scrubs (clearGlossGhostOnNextPaint): those pay
  // for a box that moves around inside one frame, not for a screen change.
  bool isTransientScreen() const override { return true; }

 private:
  std::unique_ptr<Page> page;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  std::string nextPageFirstWord;
  // TOC chapter title for the looked-up page, stored on enrolled flashcards.
  std::string chapterTitle_;

  WordSelectNavigator navigator;
  DictionaryLookupController controller;

  // The session dictionary override in force when this screen opened, restored in onExit()
  // and between lookups. A per-word card dictionary must not outlive the screen that applied
  // it: initGloss() resolves activeDictPath() once at onEnter, so a leaked override would open
  // the NEXT page's gloss box in the last looked-up word's dictionary. EpubReaderActivity's
  // SessionOverrideScope only clears at book close, far too late for that.
  //
  // The RAW override, not activeDictPath(): an empty capture must restore to "no override"
  // rather than pinning the configured dictionary in as an explicit one.
  std::string enterSessionDict_;
  // Whether enterSessionDict_ was a transient fallback promotion, so it is put back with the
  // lifetime it had. Same distinction DictionaryDefinitionActivity draws on entry.
  bool enterSessionDictWasPromotion_ = false;

  // Point the lookup that is about to start at the dictionary this word's flashcard records,
  // falling back to the screen's entry dictionary when it has none. Installed on the
  // controller as a pre-lookup hook, so it covers Confirm, multi-select phrases and the
  // "Did you mean?" re-lookup alike — the last of which matters because a suggestion-derived
  // card is filed under the SUGGESTION, not the word the user pointed at.
  static void preLookupTrampoline(void* ctx, const std::string& cleanedWord);
  void applyCardDictForLookup(const std::string& cleanedWord);

  // Differential repaint state. The first render in a session always goes through
  // the full path (page->render + snapshot setup). After that, cursor moves use the
  // differential path: restore previous highlight pixels, snapshot new region, draw
  // new highlight, push only the dirty rect to the panel. State is reset to FullPage
  // whenever the framebuffer is disturbed by something other than the highlight
  // (controller overlay, multi-select, hyphenated wrap).
  enum class RenderMode { FullPage, Differential };
  RenderMode nextRenderMode_ = RenderMode::FullPage;
  int prevHighlightIdx_ = -1;

  // One-shot opt-in from the caller: "the framebuffer already contains the
  // page at marginLeft/marginTop (e.g. EpubReader just rendered it before the
  // hold-to-lookup gesture)". When true, the first render() skips clearScreen
  // + page->render and overlays only the highlight + button hints. Consumed
  // (cleared) on the first render() call regardless of which branch is taken,
  // so any future full-repaint goes through the normal re-render path.
  bool framebufferContainsPage_ = false;

  // Bottom-of-screen reserved area the caller left below the page text
  // (status bar OR auto-page-turn indicator). Cleared in the skip-initial
  // fast path so the entry frame matches the menu→lookup visual state.
  int reservedBottomHeight_ = 0;

  Mode mode_ = Mode::Dictionary;

  // Initial marker band, chosen by the caller from current-page dwell (see CrossPointSettings
  // dictMarkerDwellEnabled). Defaults to Middle for callers that don't set it.
  WordSelectNavigator::InitialMarker initialMarker_ = WordSelectNavigator::InitialMarker::Middle;

  // Screen point the caller pointed at (the reader's page long-press), or -1 for "none".
  // onEnter resolves it to a word and starts there instead of at initialMarker_'s band; in
  // HighlightRange that word also becomes the range anchor, so the user only has to tap the
  // OTHER end. A point that hits no word falls back to the band.
  int initialPointX_ = -1;
  int initialPointY_ = -1;
  // Set when initialPointX_/Y_ resolved to a word in Dictionary mode: the caller pointed at
  // the word AND asked for a lookup (the reader's long-press menu), so the first loop tick
  // runs it instead of parking the cursor there and waiting for a tap that would only name
  // the word a second time.
  bool autoLookupPending_ = false;

  // See setGlossCursorMemory. glossCursorOut_ is written only for a session the gloss box ran in.
  PageMarks::PageKey markKey_;
  // Draw markKey_'s marks over the page just rendered, limited to rows in [bandTop, bandBottom).
  void drawPageMarks(int bandTop = INT16_MIN, int bandBottom = INT16_MAX) const;
  // Put a just-enrolled card's mark in the resident table, so this screen's next repaint shows it.
  void addLookupMark(const std::string& word, const std::string& excerpt) const;
  // On a button board, or with the gloss box up, a cursor step is taken only once the frame for the previous one is on
  // the panel: presses that land while it is still drawing are dropped, so a burst of Left/Right
  // costs one frame per step actually seen instead of a queue of moves racing the render.
  // Set by the loop task when a step requests a frame, cleared by the render task as it returns.
  std::atomic<bool> stepFramePending_{false};
  int glossResumeIdx_ = -1;
  int* glossCursorOut_ = nullptr;
  bool glossTracksCursor_ = false;

  // True when opened mid hold-Back (the reader's hold-Back → highlight gesture): swallow that
  // first Back release so it doesn't immediately cancel the selection. Other entry paths have
  // already released Back, so this stays false and Back works on the first tap as usual.
  bool consumeInitialBackRelease_ = false;

  // HighlightRange mode input: single Confirm tap = single-word quote; long-press +
  // move + Confirm = ranged quote. Emits a HighlightRangeResult and finishes. No-op
  // in Dictionary mode.
  void handleHighlightInput();
  void emitQuoteResult(int fromFlatIdx, int toFlatIdx, std::string previewText);

  // Confirm-button hint label, shown on the only labeled (full-height) button so the
  // user can tell the two near-identical selection modes apart: "Highlight" in
  // HighlightRange mode, "Look Up" in Dictionary mode.
  const char* confirmHintLabel() const;
  const char* backHintLabel() const;

  // Page-local sentence around the current selection, captured at lookup time to
  // store as the flashcard front-face context. "" if there is no selection.
  std::string buildLookupExcerpt() const;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  // Force the next render() to take the full-repaint path. Used after the lookup controller
  // or a sub-activity (e.g. DictionaryDefinitionActivity) has drawn directly to the
  // framebuffer outside our render(), making the snapshot/dirty-rect state stale. Without
  // this reset, the differential path would restore a small region of "page bg + word text"
  // onto a framebuffer full of unrelated content, and the next push would show that
  // unrelated content with one word's worth of correct page state overlaid.
  void forceFullRepaintOnNextRender() {
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    // Whatever drew over us also erased the box, so nothing holds one any more. Left stale,
    // restoreVacatedGlossStrip would clear and re-render a strip of a framebuffer that no
    // longer contains the page.
    if (gloss_) gloss_->drawnY = kGlossNotDrawn;
  }

  // --- Inline gloss box ------------------------------------------------------------------
  // The box sits directly above the selected row, or directly below it when the selection is
  // too near the top of the page for the box to fit above. Above is preferred because it
  // covers rows already read, where the opposite-half band this replaced could cover rows the
  // reader had not reached yet.
  //
  // A box that follows the selection has to give back the page text it was covering when it
  // moves. Doing that with page->render would cost the very thing the differential path above
  // exists to avoid, so restoreVacatedGlossStrip re-renders only the handful of page elements
  // that intersect the vacated strip. The box moves once per ROW change, not once per
  // keypress: a left/right scan along a row leaves it parked.
  static constexpr int kGlossNotDrawn = -1;

  // Everything the gloss needs, in one heap block allocated by initGloss() only when every
  // gate passes — a non-CJK session with an ordinary dictionary pays nothing at all. Allocated
  // AFTER extractWords, because the word array is the largest contiguous request this screen
  // makes (WordSelectNavigator.h:28-63) and must have first claim; a failed allocation here
  // just leaves the feature off.
  // A card's own dictionary, opened only when the selection is a card word saved from a
  // dictionary other than the book's. One at a time: a different card dictionary replaces it.
  struct GlossAltDict {
    Dictionary::LookupCtx ctx;
    HalFile dict;
    uint32_t hash = 0;  // registry nameHash this holds (or failed to open)
    bool ok = false;
  };

  // A card whose recorded dictionary is not the book's, keyed like LookupMarks (normalised FNV).
  struct GlossCardDict {
    uint32_t wordHash;
    uint32_t dictHash;
    uint16_t byteLen;
  };
  static constexpr int kMaxGlossCardDicts = 32;

  struct GlossState {
    Dictionary::LookupCtx ctx;  // .idx + page index, open for the whole session
    HalFile dict;               // .dict, ditto — a member handle, so closed in onExit()
    uint32_t bookDictHash = 0;  // registry nameHash of ctx's dictionary
    GlossCardDict cardDicts[kMaxGlossCardDicts] = {};
    int cardDictCount = 0;
    std::unique_ptr<GlossAltDict> alt;  // allocated on the first card that needs it
    bool useAlt = false;                // the current peek reads `alt`, not ctx/dict
    bool probeAlt = false;              // which dictionary `probe` below was read from
    DictGloss::GlossResult result;
    char raw[DictGloss::kPeekBytes] = {};

    int forFlatIdx = -1;    // word `result` describes; -1 = nothing peeked yet
    int forAnchorIdx = -1;  // other end of the range `result` describes; -1 = one word
    // Last whole-range probe, keyed by its flat range: the step check and the peek that follows
    // it ask about the same range, and this makes that one SD scan instead of two.
    int probeLo = -1;
    int probeHi = -1;
    Dictionary::PrefixProbe probe;
    // The range phrase `result` was peeked for, named in the box when it misses. Empty for a
    // single word.
    char missPhrase[40] = "";
    int y = 0;                    // top edge the box belongs at for the current selection
    bool place = false;           // false = neither side fits this selection; show no box
    int drawnY = kGlossNotDrawn;  // top edge of the box the framebuffer currently holds

    // Fixed for the session by initGloss(): neither orientation nor the page margins can change
    // while this activity is alive.
    int x = 0;
    int width = 0;
    int topY = 0;         // first y the box may occupy
    int bottomLimit = 0;  // first y the bottom chrome occupies; the box must end above it
    int minTextRoom = 0;  // page text that must stay visible outside the box (2 reader rows)

    // Follow the definition font, so they are re-derived by resolveGlossFont() rather than
    // fixed: the size can come and go mid-session (see that function).
    int fontId = 0;
    int lineHeight = 0;
    int height = 0;
    int ascender = 0;      // the enlarged cell's own height unit
    int maxCellScale = 0;  // largest replication factor the box's height allows
    bool fits = false;     // false = the box would not leave enough page visible; draw nothing

    // Enlarged-character columns, re-decided on every peek because they depend on the token as
    // well as the font. cellScale == 0 is the flowed layout — the whole box is text, exactly as
    // it was before the columns existed, and every gate below falls back to it.
    uint32_t tokenCp = 0;    // selected character, when it is a single CJK codepoint
    uint32_t variantCp = 0;  // the entry's script variant, when it differs and there is room
    bool dropField = false;  // the entry's headword field is a cell, or repeats the token: not text
    int cellScale = 0;
    int cellCount = 0;
    int cellW = 0;      // one cell; both are drawn the same width so the glyphs line up
    int textX = 0;      // left edge of the text column
    int wrapWidth = 0;  // usable width inside it
  };
  std::unique_ptr<GlossState> gloss_;

  // Page-composition counters filled by extractWords, used by initGloss's CJK gate. Counting
  // there rather than re-scanning costs nothing: the tokeniser already computes both.
  int selectableTokenCount_ = 0;
  int cjkTokenCount_ = 0;

  // Open the dictionary handles, compute the band geometry and allocate gloss_ — or leave it
  // null, which is the "feature off, behave exactly as before" state every gate falls back to.
  void initGloss();

  // Longest a CJK range may grow to with the gloss box up. Real entries run to 7 (中华人民共和国).
  static constexpr int kMaxGlossRangeChars = 8;

  // True when every token in [lo, hi] is CJK: the only ranges the cap and the partial fallback
  // apply to.
  bool rangeIsCjk(int lo, int hi) const;

  // Whether the multi-select range [anchor, cursor] may stand with the gloss box up: false only
  // past kMaxGlossRangeChars on a CJK range. Logs the dictionary's verdict on every CJK step.
  bool glossRangeAllowed(int anchor, int cursor);

  // probePrefixIn for the whole range [lo, hi], served from GlossState's one-entry cache.
  const Dictionary::PrefixProbe& probeGlossRange(int lo, int hi, const std::string& phrase);

  // Read the entry for the multi-token range [lo, hi] (joined as `phrase`) into gloss_->raw. A CJK
  // phrase that only begins a longer headword falls back to its longest leading headword, marked
  // "~" as partial. 0 when nothing starts with the phrase.
  size_t readGlossPhraseEntry(int lo, int hi, const std::string& phrase, bool cjk);

  // One deck pass: cards saved from a dictionary other than the book's into gloss_->cardDicts,
  // and every multi-token card word printed on this page into cardRanges_.
  void loadGlossCards();

  // --- Card-word auto-select (gloss box only) --------------------------------------------
  // Landing on any token of a card word printed on this page selects the whole word, as a
  // range the navigator holds like a manual one (so the highlight, the box's phrase peek and
  // Confirm's phrase lookup all just work). A word-axis step back from it drops to its last
  // token alone and leaves the word "suppressed" until the cursor walks out of it, so the
  // tokens inside a card word stay reachable one by one.
  struct CardRange {
    int16_t lo;
    int16_t hi;
  };
  static constexpr int kMaxCardRanges = 24;
  CardRange cardRanges_[kMaxCardRanges] = {};
  int cardRangeCount_ = 0;
  int autoLo_ = -1;  // the auto-selected range, while the navigator still holds it
  int autoHi_ = -1;
  int suppressLo_ = -1;
  int suppressHi_ = -1;
  bool autoRangeActive() const {
    return autoLo_ >= 0 && navigator.isMultiSelecting() && navigator.getAnchorFlatIndex() == autoLo_;
  }
  // After a navigation step taken out of an auto range: settle the cursor as single-token select.
  void leaveAutoRange();
  // Select the card word under the cursor, if any. True when it did.
  bool maybeAutoSelectCardWord();
  // Point the peek at the card's dictionary when `token` is such a card's word, else the book's.
  void selectGlossDict(const std::string& token);
  bool openGlossAltDict(uint32_t dictHash);
  Dictionary::LookupCtx& glossCtx() { return gloss_->useAlt ? gloss_->alt->ctx : gloss_->ctx; }
  HalFile& glossDictFile() { return gloss_->useAlt ? gloss_->alt->dict : gloss_->dict; }

  // Flat index of the page's first word outside a title (a centred or wholly bold line), or -1.
  int findFirstBodyWord() const;

  // Point the box at the definition font (family + the dictionary's own point size) and re-derive
  // everything that depends on its line height. Called on every peek, not once, because the size
  // can disappear underneath us — see the comment on the definition.
  void resolveGlossFont();

  // Decide the enlarged-character columns for the token just peeked: how many cells, at what
  // replication factor, and what is left for the text column. Sets gloss_->cellScale to 0 —
  // the flowed layout — for anything that is not a single CJK codepoint, or when the cells
  // would leave too little width for a definition worth reading.
  // `entry` is the raw bytes readEntry filled, or nullptr on a miss — the variant is read from
  // there, and on a miss the buffer still holds the PREVIOUS word's entry.
  void planGlossCells(const char* token, const char* entry);

  // Peek the word at currIdx and place its box relative to the selected row. Returns true when
  // the caller must take the full-repaint path — only when the strip the box is leaving holds
  // an image, which restoreVacatedGlossStrip cannot put back cheaply.
  bool updateGloss(int currIdx, int selectionLineHeight);

  // Give the page back the strip the box has just moved off. Must run BEFORE the highlight is
  // drawn: it re-renders page lines, which would otherwise paint text over a fresh highlight.
  // No-op while the box is parked, which is every keypress except a row change.
  // Returns true when it scrubbed, which the caller records in the render log: a relocation
  // frame costs roughly twice a plain one, so the two must stay distinguishable there.
  bool restoreVacatedGlossStrip();

  // Clear `height` pixels of the box column at `y` and re-render the page elements that
  // intersect it. Over-inclusive by a line either side: redrawing a line that is already
  // correct paints identical pixels, whereas missing one leaves a white gap.
  void renderPageStrip(int y, int height);

  // True if a PageImage intersects [y, y+height). Text and rules are cheap to re-render;
  // an image may need decoding, so a strip containing one is left to the full-repaint path.
  bool stripHasImage(int y, int height) const;

  // Paint the box at gloss_->y: clear, frame, rows. No panel push — the caller's single
  // displayBuffer covers the highlight and the box in one refresh.
  void drawGloss();

  // Ask the NEXT paint to collapse the box's accumulated refresh residue. Called when the box
  // relocates (a row change, which is already a large visual change) and on the transitions out
  // of it. Never on a plain left/right step: a refresh there is a flash in the middle of a
  // stationary box the user is reading.
  void clearGlossGhostOnNextPaint();

  // Width measurement injected into DictGloss::fit. ctx is `this`. isIpa is ignored: the gloss
  // is drawn entirely in the reader font, which is the CJK-capable one and is already partly
  // resident from the page render.
  static int measureGlossWidth(void* ctx, const char* text, EpdFontFamily::Style style, bool isIpa);

  // Batched bitmap-glyph prewarm for the word at currIdx, so the upcoming
  // drawText (inside renderHighlightDifferential / renderHighlight) doesn't
  // serialize cold-miss SD reads one character at a time. No-op for invalid
  // indices or missing FontCacheManager.
  void prewarmHighlightGlyphs(int currIdx);

  // Build SdCardFont's persistent advance table for every codepoint on the
  // current page, in one batched SD operation. After this, all subsequent
  // getTextAdvanceX calls take the in-RAM binary-search fast path instead
  // of the slow per-glyph fontMap fallback (~50ms each).
  void prebuildAdvanceTable();

  // Allocation-free dry run of extractWords: how many WordInfo entries it will emit, how
  // many text-pool bytes they need, and how many rows they fall into. Lets extractWords
  // size its containers in one shot instead of growing into an abort on a CJK page, where
  // per-character tokenisation makes the word array the largest contiguous block the
  // reader asks for outside the framebuffer. Counts are exact except for dash-split
  // tokens, which are bounded high.
  void countTokens(size_t& outWords, size_t& outPoolBytes, size_t& outRows) const;

  void extractWords(std::vector<WordSelectNavigator::WordInfo>& words, std::vector<WordSelectNavigator::Row>& rows,
                    std::string& textPool);
  void mergeHyphenatedWords(std::vector<WordSelectNavigator::WordInfo>& words,
                            std::vector<WordSelectNavigator::Row>& rows, std::string& textPool);
};
