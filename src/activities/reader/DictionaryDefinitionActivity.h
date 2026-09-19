#pragma once
#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "components/OptionPopup.h"
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
                                        LookupHistory::Status historyStatus = LookupHistory::Status::NotFound,
                                        bool allowCardDelete = false)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        foundLocation(location),
        showLookupButton(showLookupButton),
        cachePath(std::move(bookCachePath)),
        recordHistory(recordHistory),
        historyWord(std::move(historyWord)),
        historyStatus(historyStatus),
        allowCardDelete_(allowCardDelete),
        controller(renderer, mappedInput, *this, cachePath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Part of the reading flow (opened from the page mid-read), so it follows the
  // reading surface's night-mode polarity like the word-select overlay.
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
  // Whether front Left may delete this word's flashcard. True for a lookup made from the reader
  // page (DictionaryWordSelectActivity), where the page underline the delete removes lives, and
  // for a card's back face during a review (FlashcardReviewActivity), where the definition is
  // what the user is judging. A review holds newest-first deck indices that FlashcardDeck::remove
  // renumbers, so that caller repairs its session when it sees the card gone -- any new caller
  // passing true owes the same check. The history and flashcard lists have their own delete.
  bool allowCardDelete_ = false;
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
  // Whether prevSessionDict_ was a transient fallback promotion rather than an explicit
  // choice, so undoing the switch restores it with the lifetime it had.
  bool prevSessionDictWasPromotion_ = false;
  // Swallows the Confirm release that follows the long-press switch, so it doesn't
  // also fall through and open word-select. Same shape as
  // WordSelectNavigator::handleMultiSelectInput's confirmReleaseConsumed.
  bool dictSwitchReleaseConsumed_ = false;

  // The override in force when this screen opened, restored in onExit(). A dictionary
  // chosen here is scoped to the word on screen: the next lookup resolves through the
  // book's configured dictionary again.
  //
  // Deliberately NOT a Dictionary::SessionOverrideScope. Three of the four screens that
  // open this one (FlashcardList, LookedUpWords, FlashcardReview) install a card's own
  // dictionary via applyCardDict BEFORE constructing us, and that scope's destructor
  // clears the override outright — which would drop the parent's choice on the way back.
  // Saving and restoring what was actually in force is correct at every call site.
  std::string enterSessionDict_;
  // Whether enterSessionDict_ was a transient fallback promotion, so it is restored with
  // the lifetime it had. Same distinction as prevSessionDictWasPromotion_ above.
  bool enterSessionDictWasPromotion_ = false;

  // The dictionary recorded on this word's flashcard, read once in onEnter(). Together these
  // distinguish the three states the Set offer depends on: no card at all (cardDictExists_
  // false, no offer), a card recording nothing (hash 0 — a legacy card, which the offer DOES
  // stand for so it can finally be stamped), and a card already naming a dictionary.
  //
  // Read once, never per render: FlashcardDeck::cardDict streams the deck off SD, and the offer
  // is evaluated on every paint. The only thing that changes it here is our own Set action,
  // which updates it in place.
  uint32_t cardDictHash_ = 0;
  bool cardDictExists_ = false;

  // The word whose definition is ON SCREEN, and its card state. Distinct from historyWord, which
  // names the word this activity was OPENED for and goes stale the moment the user chains forward
  // -- that staleness is exactly why cardActionable() stands Set and Delete down at depth > 0.
  // Add and the "xN" badge are about what is being displayed, so they need their own key, refreshed
  // wherever the displayed word changes.
  std::string currentWord_;
  bool currentHasCard_ = false;
  uint32_t currentCount_ = 1;  // deck lookup count; 1 means no badge
  // Re-read currentHasCard_/currentCount_ for currentWord_. One streaming pass over the deck, the
  // same one onEnter makes -- cheap enough per chain hop, which is a deliberate navigation.
  void probeCurrentCard();
  // True when the word on screen has no card and there is a deck to put one in. The Add offer:
  // mutually exclusive with Set and Delete by construction, since those need a card to act on.
  bool addOfferStands() const;
  // Enrol currentWord_ with the dictionary now in force. Excerpt and chapter are empty by design:
  // the sentence a word came from only exists on the reading page, and a chained word has no page.
  // Returns false when the offer does not stand, so a Right release can fall through to paging.
  bool addCurrentCard();

  // Hit rectangles of the footer controls, refreshed by render(). Zero width means "not drawn
  // this frame". Tapping the dictionary name cycles within its group and holding it opens the
  // picker; the Set chip beside it commits the one on show to the card. All of them exist
  // because the X4 Pro profile (BoardConfig.h, XTEINK_X4_PRO) leaves back/confirm/left/right
  // unassigned and wires its two physical keys to Up/Down — there is no Confirm to hold and no
  // Right to press, so touch is the only way any of these is reachable there.
  int dictLabelX_ = 0;
  int dictLabelY_ = 0;
  int dictLabelW_ = 0;
  int dictLabelH_ = 0;
  int setChipX_ = 0;
  int setChipY_ = 0;
  int setChipW_ = 0;
  int setChipH_ = 0;
  int addChipX_ = 0;
  int addChipY_ = 0;
  int addChipW_ = 0;
  int addChipH_ = 0;
  // Delete, on the same terms as the Set chip: drawn only where there is no Left button to
  // press. Its hint-strip slot is zero-height on a touch board, so without this the offer is
  // named nowhere and reachable by nothing.
  int delChipX_ = 0;
  int delChipY_ = 0;
  int delChipW_ = 0;
  int delChipH_ = 0;

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
  // drawText positions a run by the top of its cell, so its baseline lands at y + ascender.
  // The IPA font is a fixed built-in whose ascender differs from the body font's, so drawing
  // both runs at the same y puts them on two different baselines. Shift the IPA run by this
  // to land it on the body baseline. Zero when the two ids are the same font.
  int ipaBaselineOffset() const {
    return renderer.getFontAscenderSize(defFontId_) - renderer.getFontAscenderSize(ipaFontId());
  }

  // Orientation-aware layout gutters (computed in wrapText, used in render and extractWordsFromLayout)
  int leftPadding = 20;
  int rightPadding = 20;
  int hintGutterHeight = 0;
  int contentX = 0;
  int hintGutterWidth = 0;
  // Height the footer line (pagination + active dictionary) claims at the bottom
  // of the panel. It is the button-hints strip on a button board, where the
  // footer rides just above the hints; on a touch board that strip is
  // zero-height, so the line reserves its own or it lays out past the bottom
  // edge and is clipped. Shared by the layout and the draw.
  int footerReserve() const;

  int contentTop = 0;  // top of the header band, i.e. below the hint gutter + bezel + margin
  int bodyStartY = 0;  // top of the text body (set in wrapText)

  // Word-select mode (activated by pressing Look Up Word in view mode)
  // Touch boards: look up the word under a held point, chaining forward without entering
  // word-select mode. No-op when the point hits no word. See the definition for why the mode
  // itself is not entered.
  void lookupWordAtPoint(int x, int y);
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
  // Single page-turn entry point for every input route on this screen (side buttons, the
  // Left/Right aliases, the tap thirds and the vertical swipe), so they cannot drift apart.
  // Wraps at both ends via ButtonNavigator's index helpers — the same wrap every list in the
  // app uses. Returns false (having done nothing) when there is no second page: loadPage()
  // re-parses the whole definition on every turn, so a "wrap" on a one-page definition would
  // pay that plus a repaint for no visible change.
  bool turnPage(bool forward);
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
  // Swallow the Confirm release still outstanding from the long press. Shared, because
  // whichever of the two handlers runs first has to be the one that eats it.
  bool consumeDictSwitchRelease();
  // Commit a hop from registry index curIdx to nextIdx: remember the outgoing override,
  // install the new one and re-look-up the same headword. Shared by the footer tap's cycle and
  // the picker, so both arm dictSwitchInProgress_ identically.
  void applyDictSwitch(int curIdx, int nextIdx, const std::string& current);
  // Advance one dictionary within the active one's st-/other group and re-look-up the
  // headword. Returns false when there is nothing to cycle to (sole member of its group,
  // fewer than two installed, or a lookup already in flight). Still the footer tap's action,
  // and the fallback for a group the picker cannot show.
  bool cycleDictionary();
  // Fills `names` and `registryIdx` with every entry sharing curIdx's st-/other group, in
  // registry order, and returns the count -- or 0 when there is no group to offer (curIdx < 0,
  // or a lone member). `names[i]` borrows the registry entry's own string, which outlives any
  // use here. Called once to populate the picker and again to resolve the row it returns, so
  // the list can never be indexed differently than it was drawn.
  int collectDictGroup(int curIdx, const char** names, int* registryIdx, int cap) const;
  // Open the dictionary picker over the definition. Returns true when the gesture was taken --
  // including the declines, which consume it and do nothing, exactly as cycleDictionary's do.
  bool openDictPicker();
  // Allocated only while the picker is up. See the note at its definition for why this one
  // screen does not hold an OptionPopup by value the way every other host does.
  std::unique_ptr<OptionPopup> dictPicker_;
  // True when this screen may act on the word's flashcard at all: there is a card, a book to
  // hold it, and the word on screen is still the one the card is filed under. Shared by the Set
  // and Delete offers so the two cannot drift apart -- both would target the wrong card in
  // exactly the same circumstances.
  //
  // chain_.depth() == 0 is the load-bearing clause: historyWord names the word the reader
  // enrolled, and chaining forward to another word from inside a definition leaves it naming the
  // ORIGINAL. Acting then would repoint or delete a card the user is not looking at. pop()/
  // unpop() bring depth back to 0, which re-arms both offers.
  bool cardActionable() const;
  // True when the dictionary on screen differs from the one recorded on this word's card,
  // i.e. the Set offer stands. `activeHash` is the caller's already-computed active hash so
  // render() does not resolve the path twice.
  bool setOfferStands(uint32_t activeHash) const;
  // True when front Left deletes this word's card rather than turning the page back. Cheap
  // enough to call from render() -- it reads fields only, unlike setOfferStands' caller, which
  // has to resolve the active dictionary path first.
  bool deleteOfferStands() const { return allowCardDelete_ && cardActionable(); }
  // Confirm, then permanently delete this word's card. The card is what anchors the word's
  // underline on the reader page (see LookupMarks), so this is how a reader removes that mark
  // mid-read. The LookupHistory entry is deliberately left alone: the word stays in the
  // looked-up list, and looking it up again re-enrolls a fresh card.
  void promptDeleteCard();
  // Commit the dictionary on show to this word's flashcard. Deliberate and explicit —
  // switching to read a second opinion must never rewrite the card by itself.
  // Returns false without doing anything when the offer does not stand, so the Right press
  // that reached it falls through and still turns the page.
  bool setCardDictToActive();
  // Undo a switch whose re-lookup failed or was cancelled, so the dictionary named in
  // the footer still matches the definition left on screen. No-op unless a switch is
  // pending — an ordinary word lookup that comes back not-found must not clear an
  // override the user set earlier.
  void revertDictSwitchIfPending();
  // Undo a back-navigation that never produced a definition. The chain entry is popped
  // before the re-lookup starts, so a miss or a cancel would otherwise consume that level
  // AND leave chainBackNavInProgress set — which makes the next forward lookup restore
  // this entry's stale page and history index instead of opening at page 0. No-op unless
  // a back-navigation is pending. Same shape as revertDictSwitchIfPending() above.
  void restoreChainBackIfPending();
  int getLineHeight() const;
};
