#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Hold threshold for the long-press "remove from recents" action (matches RecentBooksActivity).
constexpr unsigned long RECENT_LONG_PRESS_MS = 1000;
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  if (hasReadingStats) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  // Trace cover thumb generation to SD: on the X3 (no serial) some covers fail to
  // render and the failure is otherwise silent (placeholder shown). Per-book result
  // + heap below shows whether it's low-heap, a decode error, or bad dimensions.
  SdDebugLog::setEnabled(true);

  // B-investigation (heap fragmentation): the cover JPEG decode needs a ~26.6KB
  // contiguous block, but the X4's largest free block tops out ~28660 even with
  // ~48KB total free — so covers sit on the edge of the guard. Dump the full
  // free-block picture once per cover pass (free / largest / block-count / min-ever)
  // so the cause — which allocation pins max-contiguous — can be traced from SD
  // (X3 has no serial). Grep "FRAG" in opds_debug.txt.
  {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    SdDebugLog::log("FRAG", "cover-pass free=%u largest=%u blocks=%u minFreeEver=%u", (unsigned)info.total_free_bytes,
                    (unsigned)info.largest_free_block, (unsigned)info.free_blocks, (unsigned)info.minimum_free_bytes);
  }

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (Storage.exists(coverPath.c_str())) {
        // Thumb already present — drawCoverTile renders it.
      } else if (FsHelpers::hasEpubExtension(book.path)) {
        Epub epub(book.path, "/.crosspoint");
        // Skip loading css since we only need metadata here
        epub.load(false, true);

        // Try to generate thumbnail image for Continue Reading card
        if (!showingLoading) {
          showingLoading = true;
          popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        }
        GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
        // Don't wipe the persisted cover path on failure: thumb generation can fail
        // transiently (heap fragmentation), and clearing it would blank the cover
        // until the book is reopened. drawCoverTile falls back to a placeholder when
        // the thumb file is absent; a later attempt (fresh heap) regenerates it.
        const bool ok = epub.generateThumbBmp(coverHeight);
        SdDebugLog::log("COVER", "epub thumb %s: free=%u largest=%u path=%s", ok ? "ok" : "FAILED",
                        (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                        book.path.c_str());
        coverRendered = false;
        requestUpdate();
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        // Handle XTC file
        Xtc xtc(book.path, "/.crosspoint");
        if (xtc.load()) {
          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          // See note above: don't wipe the cover path on a (possibly transient) failure.
          const bool ok = xtc.generateThumbBmp(coverHeight);
          SdDebugLog::log("COVER", "xtc thumb %s: free=%u largest=%u path=%s", ok ? "ok" : "FAILED",
                          (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                          book.path.c_str());
          coverRendered = false;
          requestUpdate();
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  // Brief heap probe (struct embeds a ~785-byte ReadingTimeHistory — never a
  // stack local) just to decide whether the menu entry should be shown.
  hasReadingStats = false;
  if (auto globalStats = makeUniqueNoThrow<GlobalReadingStats>()) {
    hasReadingStats = GlobalReadingStats::load(*globalStats) && globalStats->totalReadingSeconds > 0;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE
                      ? 0
                      : base + menuItemToIndex(initialMenuItem, hasOpdsServers, hasReadingStats);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();

  const size_t total = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (total == 0) return false;

  // Split into horizontal strips so each malloc is small enough to fit a
  // fragmented heap (one big contiguous alloc fails after the reader chops the
  // heap up). Byte size scales ~linearly with logical height in every
  // orientation, so equal row-strips give roughly equal-size chunks.
  int count = static_cast<int>((total + COVER_CHUNK_TARGET_BYTES - 1) / COVER_CHUNK_TARGET_BYTES);
  if (count < 1) count = 1;
  if (count > COVER_MAX_CHUNKS) count = COVER_MAX_CHUNKS;
  const int stripH = (coverRectH + count - 1) / count;  // rows per chunk

  for (int i = 0; i < count; i++) {
    const int y = coverRectY + i * stripH;
    const int h = std::min(stripH, coverRectY + coverRectH - y);
    if (h <= 0) {
      count = i;  // exact partition consumed the region early
      break;
    }
    const size_t sz = renderer.getRegionByteSize(coverRectX, y, coverRectW, h);
    auto* chunk = static_cast<uint8_t*>(malloc(sz));
    if (!chunk) {
      LOG_ERR("HOME", "OOM: cover chunk %d/%d (%u bytes)", i + 1, count, (unsigned)sz);
      freeCoverBuffer();
      return false;
    }
    if (!renderer.copyRegionToBuffer(coverRectX, y, coverRectW, h, chunk, sz)) {
      free(chunk);
      freeCoverBuffer();
      return false;
    }
    coverChunks[i] = chunk;
    coverChunkSizes[i] = sz;
  }
  coverChunkCount = count;
  coverChunkStripH = stripH;
  return coverChunkCount > 0;
}

bool HomeActivity::restoreCoverBuffer() {
  if (coverChunkCount <= 0 || coverChunkStripH <= 0 || coverRectW <= 0 || coverRectH <= 0) return false;
  // Recompute the same strip partition store used and blit each chunk back.
  for (int i = 0; i < coverChunkCount; i++) {
    const int y = coverRectY + i * coverChunkStripH;
    const int h = std::min(coverChunkStripH, coverRectY + coverRectH - y);
    if (h <= 0) break;
    if (!coverChunks[i]) return false;
    if (!renderer.copyBufferToRegion(coverRectX, y, coverRectW, h, coverChunks[i], coverChunkSizes[i])) return false;
  }
  return true;
}

void HomeActivity::freeCoverBuffer() {
  for (int i = 0; i < COVER_MAX_CHUNKS; i++) {
    if (coverChunks[i]) {
      free(coverChunks[i]);
      coverChunks[i] = nullptr;
    }
    coverChunkSizes[i] = 0;
  }
  coverChunkCount = 0;
  coverChunkStripH = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  // After a long-press fired, swallow input until Confirm is physically released so the
  // release doesn't also open the book (re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // After hold-Back fired, swallow input until Back is physically released.
  if (backLongPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
      backLongPressFired = false;
    }
    return;
  }

  // Hold Back on the home screen: move the selector to the first recent book.
  if (!recentBooks.empty() && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= RECENT_LONG_PRESS_MS) {
    backLongPressFired = true;
    if (selectorIndex != 0) {
      selectorIndex = 0;
      requestUpdate();
    }
    return;
  }

  // Long-press Confirm on a recent book: prompt to remove it from the recent list.
  if (selectorIndex < static_cast<int>(recentBooks.size()) &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= RECENT_LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveRecentBook(recentBooks[selectorIndex].path, recentBooks[selectorIndex].title);
    return;
  }

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card). backPressSeen guards against the stale
  // release of the Back press that closed the previous activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
      switch (indexToMenuItem(menuIndex, hasOpdsServers, hasReadingStats)) {
        case HomeMenuItem::FILE_BROWSER:
          onFileBrowserOpen();
          break;
        case HomeMenuItem::RECENTS:
          onRecentsOpen();
          break;
        case HomeMenuItem::OPDS_BROWSER:
          onOpdsBrowserOpen();
          break;
        case HomeMenuItem::READING_STATS:
          onReadingStatsOpen();
          break;
        case HomeMenuItem::FILE_TRANSFER:
          onFileTransferOpen();
          break;
        case HomeMenuItem::SETTINGS_MENU:
          onSettingsOpen();
          break;
        default:
          break;
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (hasReadingStats) {
    const size_t pos = hasOpdsServers ? 3 : 2;
    menuItems.insert(menuItems.begin() + pos, tr(STR_READING_STATS));
    menuIcons.insert(menuIcons.begin() + pos, Chart);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::promptRemoveRecentBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }
    if (!RECENT_BOOKS.removeByPath(path)) {
      return;
    }
    // Refresh the recents list and drop the cached cover so the home tile redraws.
    freeCoverBuffer();
    coverBufferStored = false;
    coverRendered = false;
    recentsLoaded = false;
    recentsLoading = false;
    const auto& metrics = UITheme::getInstance().getMetrics();
    loadRecentBooks(metrics.homeRecentBooksCount);
    const int menuCount = getMenuItemCount();
    if (selectorIndex >= menuCount) {
      selectorIndex = menuCount - 1;
    }
    if (selectorIndex < 0) {
      selectorIndex = 0;
    }
    requestUpdate(true);
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onReadingStatsOpen() { activityManager.goToReadingStats(); }
