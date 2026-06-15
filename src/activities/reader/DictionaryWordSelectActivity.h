#pragma once
#include <Epub/Page.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictionaryLookupController.h"
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
      WordSelectNavigator::InitialMarker initialMarker = WordSelectNavigator::InitialMarker::Middle)
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
        initialMarker_(initialMarker) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::unique_ptr<Page> page;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  std::string nextPageFirstWord;

  WordSelectNavigator navigator;
  DictionaryLookupController controller;

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
  }

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

  void extractWords(std::vector<WordSelectNavigator::WordInfo>& words, std::vector<WordSelectNavigator::Row>& rows,
                    std::string& textPool);
  void mergeHyphenatedWords(std::vector<WordSelectNavigator::WordInfo>& words,
                            std::vector<WordSelectNavigator::Row>& rows, std::string& textPool);
};
