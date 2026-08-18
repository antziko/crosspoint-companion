#pragma once
#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "fontIds.h"  // IPA_FONT_ID, used by ipaFontId() below
#include "util/DictLayout.h"
#include "util/DictionaryLookupController.h"
#include "util/IpaUtils.h"
#include "util/LookupChain.h"
#include "util/LookupHistory.h"
#include "util/WordSelectNavigator.h"

class DictionaryDefinitionActivity final : public Activity {
 public:
  // showLookupButton=true:
  //   Confirm = enter word-select mode on the definition text (Look Up Word).
  //   Back (short press) = return to caller (isCancelled=true).
  //   Back (long press, >= LONG_PRESS_MS) = Done — exit to reader (isCancelled=false).
  // showLookupButton=false:
  //   Back/Confirm both return to caller (isCancelled=true). Unchanged from old behaviour.
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::string& headword, const DictLocation& location,
                                        bool showLookupButton = false, std::string bookCachePath = "",
                                        bool recordHistory = false, std::string historyWord = "",
                                        LookupHistory::Status historyStatus = LookupHistory::Status::NotFound)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        foundLocation(location),
        showLookupButton(showLookupButton),
        cachePath(std::move(bookCachePath)),
        recordHistory(recordHistory),
        historyWord(std::move(historyWord)),
        historyStatus(historyStatus),
        controller(renderer, mappedInput, *this, cachePath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Part of the reading flow (opened from the page mid-read), so it follows the
  // reading surface's night-mode polarity like the word-select overlay.
  bool appliesNightMode() const override { return true; }
  // The manual screen refresh blanks the framebuffer from the main loop, so the snapshot the
  // in-definition word-select path would restore describes pixels that are no longer there.
  void onFramebufferInvalidated() override {
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
  }

 private:
  std::string headword;
  DictLocation foundLocation;
  bool showLookupButton;
  std::string cachePath;
  bool recordHistory;
  std::string historyWord;
  LookupHistory::Status historyStatus;
  // Cross-definition back-navigation stack (compact: history-index + page per
  // entry, not owned strings). pendingBack_ carries the popped entry from the
  // Back keypress to the async FoundDefinition that completes the re-lookup.
  LookupChain chain_;
  LookupChain::Entry pendingBack_{};
  bool chainBackNavInProgress = false;

  // Long-press Confirm cycles the session dictionary and re-looks-up the SAME headword.
  // The user has not navigated to a new word, so the FoundDefinition handler must skip
  // chain_.onForward — otherwise every switch would push a bogus back-entry. Unlike
  // chainBackNavInProgress this restores no page and touches no history index.
  bool dictSwitchInProgress_ = false;
  // The override in force before the current switch, so a switch that lands on a
  // dictionary without the word can be undone when the user dismisses the not-found
  // popup. Without it the footer would name dictionary B while the body still shows
  // dictionary A's definition.
  std::string prevSessionDict_;
  // Swallows the Confirm release that follows the long-press switch, so it doesn't
  // also fall through and open word-select. Same shape as
  // WordSelectNavigator::handleMultiSelectInput's confirmReleaseConsumed.
  bool dictSwitchReleaseConsumed_ = false;

  // Resident page representation (Stage 2b-pool). Segments reference text by
  // {offset, len} into pagePool_ instead of owning a std::string each — the
  // Wrapper already merged same-style runs, so each segment is one pooled,
  // null-terminated entry (kerning preserved, valid for C-API drawText).
  struct PooledSegment {
    uint16_t offset = 0;  // into pagePool_
    uint16_t len = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool isIpa = false;
  };
  struct PooledLine {
    std::vector<PooledSegment> segments;
    uint8_t indentLevel = 0;
    bool isListItem = false;
  };

  // layoutLines holds ONLY the current page's lines (Stage 2a streaming). render
  // and extractWordsFromLayout index it from 0; loadPage() refills it per turn.
  // pagePool_ backs all segment text for the resident page.
  std::vector<PooledLine> layoutLines;
  std::string pagePool_;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;

  // Reused across page turns (3.1-A): avoids re-allocating the renderer object +
  // its parser/buffers on every loadPage. A value member is fine — the activity
  // is heap-allocated, so this lives on the heap. reset()+re-feed each turn (NOT
  // kept alive mid-parse; that is the won't-fixed 2c).
  DictHtmlRenderer htmlRenderer_;

  // Page-collector state (used by collectLineSink during a wrap pass): keep only
  // collectTargetPage_'s lines into layoutLines, counting all lines produced.
  int collectTargetPage_ = 0;
  int collectLineCount_ = 0;
  // Set when a line could not be pooled for lack of heap. The wrap always runs to completion
  // regardless (collectLineCount_ still counts every line produced, so pagination stays
  // correct); this only records that the resident page is short some lines, so the render is
  // a truncated page instead of a reboot. Reset per loadPage().
  bool collectOom_ = false;

  // Resolved once per definition in wrapText(). SETTINGS.getDefinitionFontId() walks the
  // SD-font resolver trampoline + registry name lookup (SdCardFontSystem.cpp:47) on every
  // call, and the layout/render/word-extract paths query it per segment and per token.
  int defFontId_ = 0;
  // True when the current definition carries markup DictHtmlRenderer can parse — HTML
  // (sametypesequence=h) or XDXF (=x). Not named defIsHtml_: both formats share one tag table,
  // and treating XDXF as non-markup is exactly the bug that drew its tags on screen as text.
  // Dictionary::readInfo() is an SD open+read and loadPage() runs on every page turn, so it is
  // resolved once alongside defFontId_.
  bool defIsMarkup_ = false;

  // True only when prewarmDefinitionFont() confirmed the IPA font's glyphs are resident.
  // The IPA font is a built-in compressed font whose non-prewarmed draw path inflates a whole
  // ~11KB group per glyph (FontDecompressor.cpp:182); on a fragmented heap that allocation
  // fails and the glyph is silently DROPPED, which is what device logs showed as blank
  // phonetics. When this is false the IPA segments are drawn in the body font instead —
  // an SD font, whose per-glyph loads are small and can never make that request.
  // Decided before loadPage() so measurement and drawing agree; read only via ipaFontId().
  // Defaults true for the built-in-body-font case, which returns from prewarmDefinitionFont()
  // before the gate ever runs: those fonts have a different heap profile and used the IPA font
  // unconditionally before this existed. The SD path resets it to false and re-earns it.
  bool ipaWarm_ = true;
  // Font the IPA segments are measured AND drawn with. Both must use this: picking the font
  // differently in the two passes lays out widths for glyphs that are never drawn.
  int ipaFontId() const { return ipaWarm_ ? IPA_FONT_ID : defFontId_; }

  // Orientation-aware layout gutters (computed in wrapText, used in render and extractWordsFromLayout)
  int leftPadding = 20;
  int rightPadding = 20;
  int hintGutterHeight = 0;
  int contentX = 0;
  int hintGutterWidth = 0;
  int contentTop = 0;  // top of the header band, i.e. below the hint gutter + bezel + margin
  int bodyStartY = 0;  // top of the text body (set in wrapText)

  // Word-select mode (activated by pressing Look Up Word in view mode)
  bool isWordSelectMode = false;
  WordSelectNavigator navigator;
  DictionaryLookupController controller;

  // Differential repaint state for in-definition word-select mode. Only consulted
  // when isWordSelectMode is true; reset on every view-mode render.
  enum class RenderMode { FullPage, Differential };
  RenderMode nextRenderMode_ = RenderMode::FullPage;
  int prevHighlightIdx_ = -1;

  // What opening THIS definition cost, for the on-screen diagnostic readout and nothing else.
  // openStartMs_ is stamped at the two points a new definition begins — onEnter() and the
  // chained-lookup re-wrap — and deliberately not on a page turn, so the figure stays pinned to
  // the open instead of being overwritten by the ~0.5 s a turn takes. openMs_ is set once, at
  // the end of the first render that completes after each stamp, and covers the panel refresh
  // and the AA pass; that is also why it can only be shown from the following render onwards.
  unsigned long openStartMs_ = 0;
  unsigned long openMs_ = 0;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  void wrapText();
  // Load every glyph this definition needs — IPA runs first, then the body font — once per
  // definition, BEFORE any measuring happens. Without it each body codepoint falls through
  // to SdCardFont::onGlyphMiss (one file open + 2 seeks + 2 reads per glyph through an
  // 8-slot ring) and every IPA glyph needs an ~11KB contiguous group buffer it cannot get
  // once the body prewarm has run. No-op for built-in body fonts (they cache lazily).
  void prewarmDefinitionFont();
  // Re-parse the definition and lay out ONLY page `page` into layoutLines,
  // discarding other pages as they are produced; also recomputes totalPages.
  void loadPage(int page);
  void wrapHtml();
  void wrapPlain();
  void extractWordsFromLayout();
  int getMixedWidth(std::vector<IpaTextSpan>& ipaRuns, const char* text, EpdFontFamily::Style style);
  // Width measurement adapter injected into DictLayout::wrapSpans. ctx is `this`.
  static int measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style, bool isIpa);
  // Line sink injected into DictLayout::wrapSpans: keeps collectTargetPage_'s
  // lines, counts the rest. ctx is `this`.
  static void collectLineSink(void* ctx, DictLayout::LayoutLine&& line);
  // Span sink bridge: forwards each streamed span from DictHtmlRenderer into the
  // DictLayout::Wrapper. ctx is the Wrapper*.
  static void feedSpanToWrapper(void* ctx, const StyledSpan& span);
  // Span sink for the prewarm scan pass: collects unique codepoints + styles instead
  // of measuring. ctx is a PrewarmCollector* (file-local to the .cpp).
  static void collectSpanForPrewarm(void* ctx, const StyledSpan& span);
  bool handleLongPressExitAll(bool enabled);
  // Long-press Confirm in view mode: advance the session dictionary and re-run the
  // current headword against it. Returns true when the gesture fired or its trailing
  // release was consumed (caller must return from loop()).
  bool handleDictSwitch();
  // Undo a switch whose re-lookup failed or was cancelled, so the dictionary named in
  // the footer still matches the definition left on screen. No-op unless a switch is
  // pending — an ordinary word lookup that comes back not-found must not clear an
  // override the user set earlier.
  void revertDictSwitchIfPending();
  int getLineHeight() const;
};
