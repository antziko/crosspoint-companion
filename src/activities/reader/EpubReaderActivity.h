#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "BookReadingStats.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

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
  bool showBookmarkMessage = false;
  bool bookmarkMessageRemoved = false;   // false = "added", true = "removed" text
  bool bookmarkMessageReturn = false;    // true = "return mark added" (overrides added text)
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
  // Set to millis() after each full page render; cleared to 0 while a subactivity is active.
  // Forward pageTurn measures elapsed time here for pace estimation.
  unsigned long pageShownAtMs = 0UL;
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
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void saveOrientation() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  // Adds the idle excess of a just-ended page view (dwellMs on screen) to
  // sessionIdleExcessSecs when the idle-page cap is enabled. No-op when the cap is Off
  // or the dwell is within PAGE_IDLE_THRESHOLD_SECONDS.
  void accountIdleExcess(unsigned long dwellMs);
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
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
