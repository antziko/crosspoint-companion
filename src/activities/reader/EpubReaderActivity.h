#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "BookReadingStats.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "util/WordSelectNavigator.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  // True right after this render rebuilt the section cache from scratch. If a
  // page then STILL fails to load, the cache isn't the problem — stop instead of
  // clearing + rebuilding forever (the old behavior looked "stuck on Indexing").
  bool sectionJustRebuilt = false;
  // Spine index whose cache build (createSectionFile) failed — e.g. SD write
  // error / card full. Prevents re-entering the build every frame (the endless
  // "Indexing" loop). Cleared when a chapter builds/loads OK.
  int buildFailedSpine = -1;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool ignoreBackUntilRelease = false;    // Suppress Back bleed-through after dictionary chain exit
  bool ignoreNextConfirmRelease = false;  // Suppress menu open after hold-Confirm gesture fires
  bool highlightHoldFired = false;        // One-shot guard: hold-Back launched highlight, until Back released
  bool showBookmarkMessage = false;
  bool bookmarkMessageRemoved = false;  // false = "added", true = "removed" text
  bool bookmarkMessageReturn = false;   // true = "return mark added" (overrides added text)
  // Set when the bookmark action used a light (windowed) refresh instead of a
  // full re-render. The showBookmarkMessage flag is still set as a debounce timer
  // so hold-Confirm doesn't re-fire every tick; this flag prevents the dismiss
  // from calling requestUpdate() and prevents the popup from drawing.
  bool bookmarkMessageLightRefresh = false;
  bool showNoDictionaryMessage = false;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  unsigned long noDictionaryMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  BookReadingStats readingStats;
  unsigned long sessionStartMs = 0UL;
  // Wall-clock instant the reader was suspended by a pushed sub-activity (menu, word-select,
  // chapter select, stats, ...). 0 = not paused. onResume() shifts sessionStartMs forward by
  // this gap so time spent in sub-screens is not counted as reading. See onPause/onResume.
  unsigned long sessionPauseStartMs = 0UL;
  // Set to millis() after each full page render; cleared to 0 while a subactivity is active.
  // Forward pageTurn measures elapsed time here for pace estimation.
  unsigned long pageShownAtMs = 0UL;
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

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
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
  void openWordSelect(bool framebufferContainsPage);
  // Highlight (hold-Back) entry point. If the current page already has a quote, shows a
  // confirm dialog (existing text + Delete/Add-new/Cancel) and acts on the choice; with
  // no existing quote, launches the selection directly.
  void openHighlightSelect();
  // Launch word-select in HighlightRange mode; saves the returned range as a quote via
  // BookmarkStore::addQuote. Shared by the no-existing-quote path and the "Add new" choice.
  void launchHighlightWordSelect();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void saveOrientation() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // Adds the idle excess of a just-ended page view (dwellMs on screen) to
  // sessionIdleExcessSecs when the idle-page cap is enabled. No-op when the cap is Off
  // or the dwell is within PAGE_IDLE_THRESHOLD_SECONDS.
  void accountIdleExcess(unsigned long dwellMs);
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
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
