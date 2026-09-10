#pragma once

#include <Txt.h>

#include <cstdint>
#include <vector>

#include "CrossPointSettings.h"
#include "TxtReaderMenuActivity.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // --- Per-book orientation (mirrors the EPUB reader) ---
  // The active orientation is loaded from orientation.bin on enter and persisted
  // on change, independent of the global SETTINGS.orientation default.
  void loadOrientation();
  void saveOrientation() const;
  // Re-apply a new orientation: persist, rotate the renderer, and force a page
  // re-index (viewport width may change). Reading position is preserved as a
  // fraction across the reflow. No-op if unchanged.
  void applyOrientation(uint8_t orientation);
  bool restorePendingFraction = false;
  float pendingProgressFraction = 0.0f;

  // --- Reader menu ---
  void openReaderMenu();
  void onReaderMenuConfirm(TxtReaderMenuActivity::MenuAction action);
  void jumpToPercent(int percent);
  bool ignoreBackUntilRelease = false;  // suppress Back bleed-through after a sub-activity exits

  // --- Bookmarks ---
  void toggleBookmark();
  void openBookmarks();
  // In-memory cache of bookmarked page indices so the status-bar icon can be
  // drawn each render without re-reading the SD store. Refreshed on enter, on
  // toggle, and after the bookmark viewer (which can delete entries).
  std::vector<uint32_t> bookmarkPages;
  void reloadBookmarkPages();
  bool isCurrentPageBookmarked() const;

  // --- Auto page turn ---
  void toggleAutoPageTurn(uint8_t option);
  bool automaticPageTurnActive = false;
  unsigned long lastPageTurnTime = 0;
  unsigned long pageTurnDuration = 0;
  uint8_t selectedPageTurnOption = 0;

  // --- Screenshot ---
  bool pendingScreenshot = false;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
