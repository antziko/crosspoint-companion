#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>

#include "BookReadingStats.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "SyncScope.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"  // Rect (indexing popup progress bar)
#include "util/Dictionary.h"              // Dictionary::SessionOverrideScope member
#include "util/WordSelectNavigator.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  // Clears any session dictionary override (set by long-press Confirm on the definition
  // screen) when this activity is destroyed. This is the one that defines "session" for
  // the feature: a switched dictionary lasts for the rest of the book and reverts to the
  // configured selection on close. RAII rather than a call in onExit(): activities are
  // heap-allocated and deleted on exit, so the destructor always runs.
  Dictionary::SessionOverrideScope dictOverrideScope_;
  // True right after this render rebuilt the section cache from scratch. If a
  // page then STILL fails to load, the cache isn't the problem — stop instead of
  // clearing + rebuilding forever (the old behavior looked "stuck on Indexing").
  bool sectionJustRebuilt = false;
  // Spine index whose cache build (createSectionFile) failed — e.g. SD write
  // error / card full. Prevents re-entering the build every frame (the endless
  // "Indexing" loop). Cleared when a chapter builds/loads OK.
  int buildFailedSpine = -1;
  int currentSpineIndex = 0;
  // Hash of the current chapter's TOC title, memoised per spine item: it keys this page's
  // looked-up words in LookupMarks, and the render path must not build a std::string per frame.
  // -1 spine = not computed yet.
  int chapterHashSpine = -1;
  uint32_t chapterHash = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // A turn that arrived while a render was in flight (or inside the debounce
  // gap), latched instead of dropped: -1 back, +1 forward, 0 none. Holds at
  // most one turn — mashing collapses to the latest direction — and is
  // executed by loop() once the render task is idle again.
  int8_t pendingManualTurn = 0;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  // Paragraph anchor to land on once the target section is built, taking precedence over
  // pendingSpineProgress. A bookmark's progress is a page-count fraction captured when it
  // was made, so re-pagination (font size, margins, line spacing, rotation) moves it;
  // the paragraph index names the actual text and survives. UINT16_MAX means unavailable
  // (legacy bookmark, or a section whose cache carries no paragraph map) and the
  // percentage jump then applies unchanged.
  uint16_t pendingParagraphAnchor = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool ignoreBackUntilRelease = false;    // Suppress Back bleed-through after dictionary chain exit
  bool ignoreNextConfirmRelease = false;  // Suppress menu open after hold-Confirm gesture fires
  // Idle-time glyph prewarm: after a page settles, scan the LIKELY next page
  // (scan mode draws nothing) and load its missing glyphs from SD during idle,
  // so the next turn's in-render prewarm is a cache hit instead of ~100 ms of
  // SD reads on the page-turn critical path. One attempt per position.
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool highlightHoldFired = false;  // One-shot guard: hold-Back launched highlight, until Back released
  bool showBookmarkMessage = false;
  bool bookmarkMessageRemoved = false;  // false = "added", true = "removed" text
  bool bookmarkMessageReturn = false;   // true = "return mark added" (overrides added text)
  // Set when the bookmark action used a light (windowed) refresh instead of a
  // full re-render. The showBookmarkMessage flag is still set as a debounce timer
  // so hold-Confirm doesn't re-fire every tick; this flag prevents the dismiss
  // from calling requestUpdate() and prevents the popup from drawing.
  bool bookmarkMessageLightRefresh = false;
  bool showNoDictionaryMessage = false;
  // Inline-review tally toast, drawn over the page the reader repaints on return from a
  // review. Pre-formatted into a fixed buffer at dismissal time rather than re-snprintf'd
  // per render pass, and sized for "%d/%d correct, %d mastered" in any translation.
  bool showInlineReviewMessage = false;
  unsigned long inlineReviewMessageTime = 0UL;
  char inlineReviewMessage_[64] = {};
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  unsigned long noDictionaryMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Armed in onEnter() when the "sync prompt on open" gate passes; consumed once in loop() after
  // the first page renders, to show the open/wake sync prompt without blocking the initial paint.
  bool openSyncPromptArmed_ = false;
  // One-shot: after the open sync prompt is dismissed with Skip, swallow the page-turn (and Back)
  // that the answering button release would otherwise bleed into the resumed reader. Cleared once
  // all navigation buttons are released.
  bool suppressPageTurnUntilRelease_ = false;

  BookReadingStats readingStats;
  unsigned long sessionStartMs = 0UL;
  // Wall-clock instant the reader was suspended by a pushed sub-activity (menu, word-select,
  // chapter select, stats, ...). 0 = not paused. onResume() shifts sessionStartMs forward by
  // this gap so time spent in sub-screens is not counted as reading. See onPause/onResume.
  unsigned long sessionPauseStartMs = 0UL;
  // Set to millis() after each full page render; cleared to 0 while a subactivity is active.
  // Forward pageTurn measures elapsed time here for pace estimation.
  unsigned long pageShownAtMs = 0UL;
  // Accumulated VISIBLE time (ms) on the current page, summed across sub-activity round-trips
  // (menu/dict/highlight). Sub-activity time itself is excluded (handled by onPause/onResume).
  // The idle-page cap is applied to this total when the page is genuinely left, so an
  // interrupted long dwell is capped just like a continuous one. Reset on a genuine new page;
  // preserved on a sub-activity resume (mirrors markerDwellStartMs / preserveMarkerDwell_).
  unsigned long currentPageVisibleMs = 0UL;
  // Anchor for the dictionary/highlight marker-by-dwell feature: when the current page first
  // became visible. Unlike pageShownAtMs (reset whenever a subactivity opens), this survives a
  // word-select round-trip so re-triggering on the same page continues accumulating dwell rather
  // than restarting. Re-anchored to millis() on every genuine new-page render; preserved across a
  // word-select launch via preserveMarkerDwell_.
  unsigned long markerDwellStartMs = 0UL;
  // One-shot: the next page render is a return from word-select (not a new page), so it must
  // resume (not re-anchor) the marker dwell. Set via pauseMarkerDwell() at the launch sites.
  bool preserveMarkerDwell_ = false;
  // Reading ms accumulated on the current page at the moment word-select opened. On return the
  // dwell anchor is shifted so the time spent inside word-select (not reading) is excluded.
  unsigned long markerDwellPausedElapsedMs = 0UL;
  // Progress-save debounce state. lastSaved{Spine,Page}_ track what is currently persisted in
  // /progress.bin (-1 = unknown). turnsSinceProgressSave_ counts position changes since the last
  // write; when it reaches SETTINGS.PROGRESS_SAVE_PAGES[...] a write fires. onExit() flushes any
  // unsaved position so a normal exit/sleep never loses pages (only a hard power-off can).
  int lastSavedSpine_ = -1;
  int lastSavedPage_ = -1;
  uint16_t turnsSinceProgressSave_ = 0;
  // Accumulated idle-page excess (seconds) for the current session. When the idle-page
  // cap (SETTINGS.pageIdleCapSeconds) is enabled, a page held longer than
  // PAGE_IDLE_THRESHOLD_SECONDS contributes only the cap value; the excess is summed here
  // and subtracted from the wall-clock session total at onExit. Reset in onEnter.
  uint32_t sessionIdleExcessSecs = 0;
  // Reading seconds of this session already persisted by incremental checkpoints
  // (see commitReadingTime). onExit flushes only the remaining delta, so a crash
  // mid-session loses at most one checkpoint interval, not the whole session.
  uint32_t sessionCommittedSecs = 0;
  // --- Inline flashcard review, session-scoped state --------------------------------
  // The review clock counts from the moment this reading session began, not from the book's
  // lifetime odometer: opening a book always buys a full interval of undisturbed reading.
  // Both fields are deliberately RAM-only, so they reset on close and on the chip reset that
  // ends a deep sleep -- that is exactly the session boundary the schedule is meant to track.
  // Baseline is an odometer reading (readingTotalSeconds() at session start), which keeps the
  // idle-page and sub-activity exclusions that accounting already applies.
  uint32_t inlineReviewBaseSecs = 0;
  // Skips taken this session. Each one permanently lengthens the interval for the rest of the
  // session (interval x (1 + skips)), so "not now" keeps meaning "not this often" even after a
  // review is completed. Capped by INLINE_REVIEW_MAX_MULTIPLIER.
  uint16_t inlineReviewSkips = 0;
  // Ceiling on 1 + inlineReviewSkips. At the longest configurable interval (30 min) this caps
  // the effective gap at 3 hours, past which further stretching is indistinguishable from Off.
  static constexpr uint16_t INLINE_REVIEW_MAX_MULTIPLIER = 6;
  // Set by the render task on full-refresh pages (the e-ink ghost-clear cadence,
  // SETTINGS.refreshFrequency); consumed by loop() on the main task -- the same
  // context as onExit -- so all session accounting stays single-task.
  volatile bool statsCheckpointPending = false;
  // Minimum uncommitted delta for a checkpoint write. Skips per-page writes for
  // REFRESH_1 users and keeps SD write throttling sane; onExit passes 0 so the
  // final flush always lands.
  static constexpr uint32_t STATS_CHECKPOINT_MIN_SECS = 60;

  // Set after a page is rendered with the AA grayscale strip passes. Used by
  // lightStatusBarRefresh to skip the panel push on AA image pages: even a
  // windowed FAST_REFRESH applies the LUT to the full panel and gradually
  // darkens grayscale particles. Cleared at the start of each new render.
  bool lastPageUsedGrayscale = false;

  // True when the current page contains an image. No partial/fast refresh can update
  // the status-bar strip without darkening the grayscale image (any FAST_REFRESH
  // charges the LUT) or erasing unrecoverable image pixels, so bookmark toggles on
  // image pages do a full page re-render instead of the windowed light-refresh.
  // Set in renderContents().
  bool lastPageHadImages = false;

  // Next-book suggestion menu for the End-of-Book screen. Lazy: it embeds a
  // GfxRendererTarget + FreeInkApp (theme tokens by value, ~2KB), so it only
  // exists while the end screen is actually showing — created at the render
  // path's sole load site, dropped by loop() when the user pages back in.
  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  // Publication flag for the pointer above: the render task creates the object
  // and release-stores true; the main task acquire-loads before dereferencing,
  // so it never sees a partially constructed object. Cleared (main task, under
  // RenderLock) before reset.
  std::atomic<bool> endOfBookOptionsReady{false};

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  // Hash of the current chapter's TOC title, recomputed only when the spine item changes.
  // Keys the page's looked-up words; see LookupMarks.
  uint32_t currentChapterHash();
  // (Re)build the resident looked-up-word index from this book's flashcard deck. One streaming
  // pass; called at book open and on return from any screen that can change the deck.
  void reloadLookupMarks() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;

  // MEMFIX-PORT: background-build heap floor; portable
  // Skip background build ticks below this free-heap floor. The parse path grows
  // word vectors of heap strings — throwing allocations that abort() on OOM under
  // -fno-exceptions (field crash: bad_alloc in ParsedText::addWord during a
  // background tick under heap pressure). The tick is deferrable work:
  // page-turn transients free up between turns and the build resumes; the render
  // path still builds the page it actually needs regardless of this floor.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  // Fragmentation floor for the same gate: a tick passed the free-heap floor at
  // 34.7 KB free but the largest block was ~11 KB, and a parse allocation inside the
  // tick aborted anyway. Free heap says how much memory exists; maxAlloc says whether
  // any single allocation can actually have it. 16 KB also keeps the advance-table
  // batch path (16 KB scratch) viable during builds.
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Gate for a background build tick: true when the heap can take parse allocations.
  // Updates buildHeapPaused as a side effect.
  bool buildTickHeapGate();
  // True while the background build is gated on the heap floors. Lets skipLoopDelay()
  // return the loop to normal delay/power-saving during the pause: isBuilding() stays
  // true the whole time, and without this the loop would spin at full CPU speed doing
  // no build work — indefinitely, if the build context itself keeps the heap low.
  bool buildHeapPaused = false;
  // Heap floor for optional render-adjacent work (idle prewarm). Page
  // deserialization (TextBlock word vectors/strings) and glyph caching allocate
  // through throwing paths that abort() on OOM; skip deferrable work below it.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // Silent (background) next-chapter pre-index runs createSectionFile WITHOUT a
  // FrameBufferLoan -- the on-screen reading page still owns the framebuffer, so
  // this path's ZIP inflate window (a fixed 32 KB tinfl dictionary) must come
  // from the fragmented heap instead of the lent 48 KB scratch. When the largest
  // free block can't supply it the inflate init fails, thrashes 3 SD retries, and
  // logs an error for a chapter that will build fine on open (with the loan
  // active). Gate the optional pre-index on a largest-block floor above the 32 KB
  // window so it skips cleanly rather than failing loudly. Pure optimization: a
  // skipped pre-index just means the chapter lays out when the reader opens it.
  static constexpr size_t SILENT_INDEX_MIN_MAX_ALLOC = 36 * 1024;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Deadline backstop for the predictive gates above: if the blocking build-to-target still
  // hasn't produced the landing page this long after the build started, surface the popup
  // mid-build. Builds that finish under the deadline stay popup-free.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // True only during render()'s blocking build-to-target phase, until the popup has been
  // drawn. Gates showBuildPopup() so a background buildSomeMore in loop() can never draw
  // over a displayed page.
  bool buildPopupPending = false;
  // While the indexing popup is on screen for the current render() pass, its layout rect
  // (width > 0) and the last progress percent painted into its bar. Both reset at the top of
  // render(); the build-to-target chunk loops advance the bar via updateIndexingProgress() as the
  // build consumes HTML. lastIndexingPct_ throttles the (full-panel FAST) e-ink refresh so the bar
  // repaints at most once every INDEXING_PROGRESS_STEP percent instead of on every chunk.
  Rect indexingPopupRect_{};
  int lastIndexingPct_ = -1;
  static constexpr int INDEXING_PROGRESS_STEP = 10;
  // Draw the indexing popup and arm its progress bar: records the popup rect, resets the throttle,
  // and forces the replacing page onto the HALF ghost-cleanup path. Centralizes the popup draw sites.
  void drawIndexingPopup();
  // Advance the indexing popup's progress bar from the active build's byte progress (throttled).
  // No-op when the popup isn't showing, the framebuffer is on loan, or the build has no progress yet.
  void updateIndexingProgress();
  // Draw the indexing popup mid-build (deadline backstop in render()'s build-to-target loop).
  void showBuildPopup();
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  // The saved resume/reflow anchor is only valid until it has established the initial landing page.
  // Every deliberate navigation (page turn, percent jump, chapter select, href, progress change)
  // must drop it, or a background build finishing afterwards would snap the reader back. Callers
  // must already hold the RenderLock.
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  // Builds the next chapter's section cache in the background while the penultimate page of the
  // current chapter is on screen, so the forward turn into it is instant. No-op if already cached.
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Debounced progress save for the per-render path: writes only every PROGRESS_SAVE_PAGES[...]
  // position changes. Skips when the position is unchanged from the last write.
  void maybeSaveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void openReaderMenu();
  // framebufferContainsPage = true means the caller guarantees the framebuffer
  // currently shows the page at the renderer's oriented page margins. The
  // DictionaryWordSelectActivity will skip its initial clearScreen +
  // page->render in that case. Only the hold-to-lookup path can pass true;
  // the reader-menu → Lookup path must pass false because the menu has
  // overwritten the framebuffer.
  // Run SETTINGS.holdConfirmAction. Shared by the Confirm hold and the
  // Home-key hold so the two cannot drift. Returns false when the function did
  // nothing this frame (Off, or a message it already put up is still showing),
  // which tells the caller not to swallow the frame.
  bool runHoldAction();
  void openWordSelect(bool framebufferContainsPage);
  // Highlight (hold-Back) entry point. If the current page already has a quote, shows a
  // confirm dialog (existing text + Delete/Add-new/Cancel) and acts on the choice; with
  // no existing quote, launches the selection directly.
  void openHighlightSelect();
  // Launch word-select in HighlightRange mode; saves the returned range as a quote via
  // BookmarkStore::addQuote. Shared by the no-existing-quote path and the "Add new" choice.
  void launchHighlightWordSelect();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Persist the current position, release the Epub/Section to free RAM for TLS, and hand off
  // to KOReaderSyncActivity. sleepWhenDone makes the sync deep-sleep the device on success
  // instead of returning to the reader (used by the "sync before sleep" flow). Caller must
  // have confirmed KOREADER_STORE.hasCredentials(). Returns false if the pre-sync progress
  // save failed (sync not launched). Shared by the reader menu and the sleep prompt.
  bool launchKoSync(bool sleepWhenDone, SyncScope scope = SyncScope::All);
  // This book's live reading odometer: persisted totalReadingSeconds plus the uncommitted
  // effective remainder of the current session (matching commitReadingTime accounting).
  uint32_t readingTotalSeconds() const;
  // Reading seconds accrued since this book's last successful sync (odometer minus the
  // last-sync marker). Shared by the sleep + open sync prompts.
  uint32_t readingSecondsSinceLastSync() const;
  // True when reading accrued since the later of the last sync and the last prompt-Skip
  // meets the SYNC_PROMPT_MINUTES[syncPromptMinutesIdx] gate. The Skip baseline defers the
  // next prompt by one full interval instead of re-firing while still over the sync gate.
  bool syncPromptThresholdReached() const;
  // Record a "Skip" of a sync prompt: stamp lastSyncPromptSkipSeconds at the current
  // odometer and persist, so the next prompt waits a full interval. Shared by both prompts.
  void recordSyncPromptSkip();
  // Show the "sync before continuing" prompt on open/wake (Sync runs KOReaderSyncActivity then
  // returns to the reader; Skip resumes reading). Called once after the first page render.
  void showOpenSyncPrompt();

  // --- Inline flashcard review ("review while reading") ----------------------------
  // Configured review interval in minutes, or 0 when the feature is off.
  uint16_t inlineReviewIntervalMinutes() const;
  // Configured interval scaled by the skips taken this session, in minutes; 0 when off.
  uint16_t inlineReviewEffectiveMinutes() const;
  // Same, projected for one MORE skip -- what the next gap becomes if the user skips now.
  uint16_t inlineReviewSkippedMinutes() const;
  // Reading accrued this session past inlineReviewBaseSecs meets the effective interval.
  // Uses the same odometer as syncPromptThresholdReached(), so time inside sub-activities
  // (the review screen itself included) and idle-page excess are already excluded.
  bool inlineReviewThresholdReached() const;
  // Restart the session review clock. A skip also lengthens every later interval this
  // session; a completed review (including a partial one) keeps the current length.
  void recordInlineReview(bool skipped);
  // Called at the START of a forward page turn. Runs the gates cheapest-first and, if they all
  // pass, opens a short capped flashcard review over the page still on screen. Returns true if a
  // review was launched -- in which case the caller must ABANDON the turn: the review takes the
  // press, and the reader stays on its current page until the user presses forward again.
  bool maybeStartInlineReview();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void saveOrientation() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // Idle-page-cap excess (seconds) for a given visible dwell. Returns 0 when the cap is Off
  // or the dwell is within PAGE_IDLE_THRESHOLD_SECONDS. Pure — used both to account on page
  // leave and to compute the live reduction for the "This session" display.
  uint32_t computeIdleExcessSecs(unsigned long dwellMs) const;
  // Adds computeIdleExcessSecs(dwellMs) to sessionIdleExcessSecs. Called when a page is
  // genuinely left (render of a new page, or onExit).
  void accountIdleExcess(unsigned long dwellMs);
  // Folds the current visible segment (millis() - pageShownAtMs) into currentPageVisibleMs and
  // clears pageShownAtMs. Called when a sub-activity opens, so the page's dwell accumulates
  // across the round-trip instead of resetting.
  void accumulateVisibleSegment();
  // Chooses the initial word-select marker band from the current page's dwell when the
  // dictMarkerDwellEnabled setting is on. Must be called BEFORE pageShownAtMs is reset for
  // the launch. Returns Middle when the feature is off, during auto page-turn, or when no
  // dwell is known; longer (idle-adjusted) dwell -> lower band.
  WordSelectNavigator::InitialMarker computeWordSelectMarker() const;
  // Freeze the current page's marker dwell across a word-select launch: stash the reading ms
  // accrued so far and arm preserveMarkerDwell_ so the return render resumes instead of resetting.
  void pauseMarkerDwell();
  // Persist the not-yet-committed reading time of this session (book + global
  // stats, dated history) if it crosses the min-session threshold and the
  // uncommitted delta is at least minDeltaSecs. Main-task only.
  void commitReadingTime(uint32_t minDeltaSecs);
  // returnMark=true drops a session "return here" bookmark (distinct icon) used when
  // jumping to another chapter, so the user can get back to where they were.
  // lightRefresh=true performs a status-bar-only windowed panel update instead of a
  // full page re-render (use for interactive same-page toggles on image/AA pages).
  void addBookmark(bool returnMark = false, bool lightRefresh = false);

  // Redraw only the status-bar strip and push it via a windowed sub-rectangle
  // refresh, leaving the page content (including any AA images) untouched on the
  // panel. Used for interactive bookmark toggles that must not disturb the image.
  void lightStatusBarRefresh();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              int initialRefreshCountdown)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build runs: at the low-power
  // frequency a giant chapter's background rebuild stretches from ~40s to many
  // minutes, so the reader exits before it can finalize and the next open restarts
  // it from page 0. Reverts to normal power behavior the moment the build finishes,
  // and while the build is heap-paused (no work is happening, so spinning at full
  // speed would only burn battery; the paused gate still retries every loop pass).
  // The watermark window below MUST mirror the background-build gate in loop() (the
  // isPartial()/BUILD_WINDOW_AHEAD test): once a first-open build has laid out its
  // look-ahead window it parks (isBuilding() stays true but loop() stops pumping it),
  // so keying only on isBuilding() would spin at full clock indefinitely while idle on
  // a page -- doing no build work and blocking idle light-sleep. Gate on "a build tick
  // will actually run this pass" instead. Read unlocked like the other power heuristics
  // (setPowerSaving/lightSleep): a stale read costs at most one loop pass either way.
  bool skipLoopDelay() override {
    return section && section->isBuilding() && !buildHeapPaused &&
           (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
  }
  bool isReaderActivity() const override { return true; }
  bool appliesNightMode() const override { return true; }
  bool onManualSleepRequested() override;
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
