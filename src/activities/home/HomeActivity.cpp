#include "HomeActivity.h"

#include <Arduino.h>  // millis()
#include <Bitmap.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <SdCardFont.h>  // getStats() on each resident SD font, for the HOME paint line
#include <SdDebugLog.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <algorithm>
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
        // Lend the 48KB framebuffer for the duration of the thumb build so the
        // cover's zip-inflate window (32KB) claims build-scratch instead of a
        // malloc(32768) that fails on a home heap fragmented below 32KB (X3:
        // largest ~31732). The progress popup above is already flushed to the
        // persistent e-ink panel and generateThumbBmp does no rendering, so
        // lending is safe (mirrors the section-build loan in EpubReaderActivity).
        // Nesting-safe: an already-lent framebuffer yields an inert loan.
        bool ok;
        {
          GfxRenderer::FrameBufferLoan loan(renderer);
          ok = epub.generateThumbBmp(coverHeight);
        }
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
          // See note in the epub branch above: lend the framebuffer so the cover
          // inflate window uses build-scratch instead of a failing malloc(32768).
          bool ok;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            ok = xtc.generateThumbBmp(coverHeight);
          }
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
  // Strip height MUST be a multiple of 8. A logical row is one panel column after the portrait
  // rotate, so an 8-row strip is exactly one whole byte per column and getRegionByteSize has
  // nothing to snap; any other height pays padding on both ends of every strip. See the
  // measured table on COVER_CHUNK_TARGET_BYTES — this single constraint is what makes fine
  // chunking safe rather than actively harmful.
  int stripH = static_cast<int>(COVER_CHUNK_TARGET_BYTES * static_cast<size_t>(coverRectH) / total);
  stripH = ((stripH + 4) / 8) * 8;  // nearest multiple of 8, never 0
  if (stripH < 8) stripH = 8;
  int count = (coverRectH + stripH - 1) / stripH;
  while (count > COVER_MAX_CHUNKS) {
    stripH += 8;
    count = (coverRectH + stripH - 1) / stripH;
  }

  // Bail before allocating anything the heap plainly cannot cover. With 8-row alignment
  // `total` is what the strips actually consume, so this is an exact test rather than a tuned
  // one — it rejects only attempts that were already doomed. Without it a doomed attempt walks
  // most of the way through the region before failing and then frees it all, once per paint;
  // the device log caught that trough at free=5588 / minFreeEver=4508.
  //
  // The second condition is about CONTIGUITY, not fit. Fine chunks can fit an already
  // fragmented heap, but only by scattering into the small holes, and freeing them at onExit()
  // does not put the heap back — see COVER_SNAPSHOT_MIN_LARGEST_BLOCK for the measured
  // entry-to-next-activity table. That damage outlives Home and lands on whatever network
  // screen runs next, so a fragmented heap must take the SD path even when free heap is ample.
  const size_t freeNow = ESP.getFreeHeap();
  const size_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeNow < total + COVER_SNAPSHOT_FREE_FLOOR || largestNow < COVER_SNAPSHOT_MIN_LARGEST_BLOCK) {
    SdDebugLog::setEnabled(true);
    SdDebugLog::log("HOME", "snapshot SKIPPED (%s) need=%u+%u free=%u largest=%u min=%u -> SD",
                    freeNow < total + COVER_SNAPSHOT_FREE_FLOOR ? "free" : "frag", (unsigned)total,
                    (unsigned)COVER_SNAPSHOT_FREE_FLOOR, (unsigned)freeNow, (unsigned)largestNow,
                    (unsigned)COVER_SNAPSHOT_MIN_LARGEST_BLOCK);
    return storeCoverBufferToSd();
  }

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
      // To SD as well: this failure is the difference between an 11ms paint and an 880ms one
      // (see COVER_CHUNK_TARGET_BYTES), and the LOG_ERR above never reaches an X3.
      SdDebugLog::setEnabled(true);
      multi_heap_info_t info;
      heap_caps_get_info(&info, MALLOC_CAP_8BIT);
      SdDebugLog::log("HOME", "snapshot FAILED at chunk %d/%d want=%u total=%u free=%u largest=%u blocks=%u -> SD",
                      i + 1, count, (unsigned)sz, (unsigned)total, (unsigned)info.total_free_bytes,
                      (unsigned)info.largest_free_block, (unsigned)info.free_blocks);
      freeCoverBuffer();
      // Fall through to the heap-independent path rather than leaving the caller to redraw
      // everything on every later paint — that fallback is the whole ~890ms cost.
      return storeCoverBufferToSd();
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
  SdDebugLog::setEnabled(true);
  // allocated= is the sum of the per-strip sizes, which exceeds `total` by the byte-alignment
  // padding each strip pays. It is logged separately from total= precisely because that gap is
  // what a chunk-count change moves, and reading total= as the cost is how the 2048-byte
  // experiment went wrong (see COVER_CHUNK_TARGET_BYTES).
  size_t allocated = 0;
  for (int i = 0; i < count; i++) allocated += coverChunkSizes[i];
  SdDebugLog::setEnabled(true);
  SdDebugLog::log("HOME", "snapshot ok chunks=%d strip=%u total=%u allocated=%u free=%u largest=%u", count,
                  (unsigned)stripH, (unsigned)total, (unsigned)allocated, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return coverChunkCount > 0;
}

// Stream the tile region to SD one 8-row strip at a time. Heap cost is a single ~528-byte
// scratch buffer regardless of region size, which is the entire point: the RAM path fails on
// block distribution, and no chunk size fixes an arbitrary distribution.
bool HomeActivity::storeCoverBufferToSd() {
  coverSdSnapshot = false;
  if (coverRectW <= 0 || coverRectH <= 0) return false;

  const size_t stripBytes = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, COVER_SD_STRIP_ROWS);
  if (stripBytes == 0) return false;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(stripBytes);
  if (!scratch) {
    LOG_ERR("HOME", "OOM: SD snapshot scratch (%u bytes)", (unsigned)stripBytes);
    return false;
  }

  const unsigned long tStart = millis();
  // Close before any reopen of the same path (DESTRUCTOR_CLOSES_FILE only covers scope exit),
  // so the write below always starts from a truncated file.
  {
    HalFile out;
    if (!Storage.openFileForWrite("HOME", COVER_SD_PATH, out)) {
      SdDebugLog::setEnabled(true);
      SdDebugLog::log("HOME", "sd-snapshot open-for-write FAILED %s", COVER_SD_PATH);
      return false;
    }
    for (int y = coverRectY; y < coverRectY + coverRectH; y += COVER_SD_STRIP_ROWS) {
      const int h = std::min(COVER_SD_STRIP_ROWS, coverRectY + coverRectH - y);
      const size_t sz = renderer.getRegionByteSize(coverRectX, y, coverRectW, h);
      if (sz == 0 || sz > stripBytes) return false;
      if (!renderer.copyRegionToBuffer(coverRectX, y, coverRectW, h, scratch.get(), sz)) return false;
      if (out.write(scratch.get(), sz) != sz) {
        SdDebugLog::setEnabled(true);
        SdDebugLog::log("HOME", "sd-snapshot write FAILED at y=%d", y);
        return false;
      }
    }
    out.close();
  }

  coverSdSnapshot = true;
  SdDebugLog::setEnabled(true);
  SdDebugLog::log("HOME", "sd-snapshot ok strip=%d stripBytes=%u ms=%lu free=%u largest=%u", COVER_SD_STRIP_ROWS,
                  (unsigned)stripBytes, millis() - tStart, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return true;
}

bool HomeActivity::restoreCoverBufferFromSd() {
  if (!coverSdSnapshot || coverRectW <= 0 || coverRectH <= 0) return false;

  const size_t stripBytes = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, COVER_SD_STRIP_ROWS);
  if (stripBytes == 0) return false;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(stripBytes);
  if (!scratch) return false;

  HalFile in;
  if (!Storage.openFileForRead("HOME", COVER_SD_PATH, in)) return false;
  for (int y = coverRectY; y < coverRectY + coverRectH; y += COVER_SD_STRIP_ROWS) {
    const int h = std::min(COVER_SD_STRIP_ROWS, coverRectY + coverRectH - y);
    const size_t sz = renderer.getRegionByteSize(coverRectX, y, coverRectW, h);
    if (sz == 0 || sz > stripBytes) return false;
    if (in.read(scratch.get(), sz) != static_cast<int>(sz)) return false;
    if (!renderer.copyBufferToRegion(coverRectX, y, coverRectW, h, scratch.get(), sz)) return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  // SD-backed snapshot takes precedence: when it is set, the RAM chunks were never allocated.
  if (coverSdSnapshot) return restoreCoverBufferFromSd();
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
  // The SD file is left in place (it is rewritten on the next store), but the flag must clear
  // or restoreCoverBuffer would keep dispatching to a snapshot this instance no longer owns.
  coverSdSnapshot = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

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

  // Touch hold on a recent-book tile = the Confirm hold above: prompt to remove
  // it from the list. The X4 Pro has no Confirm pin at all (BoardConfig.h,
  // XTEINK_X4_PRO), so without this the gesture is unreachable there. Resolved
  // before the tap/down block: wasScreenLongPress suppresses the rest of the
  // contact, so the finger lift cannot also open the book.
  if (!recentBooks.empty()) {
    int holdX = 0;
    int holdY = 0;
    if (mappedInput.wasScreenLongPress(holdX, holdY)) {
      const int heldCount = std::min(static_cast<int>(recentBooks.size()), std::max(1, metrics.homeRecentBooksCount));
      int heldIndex = -1;
      if (GUI.recentBookIndexFromPoint(renderer, coverRect(), heldCount, holdX, holdY, heldIndex) && heldIndex >= 0 &&
          heldIndex < static_cast<int>(recentBooks.size())) {
        selectorIndex = heldIndex;
        promptRemoveRecentBook(recentBooks[heldIndex].path, recentBooks[heldIndex].title);
      }
      return;
    }
  }

  // Touch: the recent-book cover band, then the menu below it. Ported from
  // upstream #2957 — feat's home screen had no touch path at all, so on a
  // touch board the whole screen was button-only.
  //
  // Hit-testing is delegated to the theme, against the SAME rects render()
  // hands the matching draw calls, so the bands cannot drift from the visuals.
  // A metrics-derived grid here only ever matched themes that draw a vertical
  // menu stack; Vega's is a horizontal row anchored to the screen bottom.
  // Down and Tap are read independently: one frame can carry both (a held
  // contact plus a completed tap), and each reports its own coordinates.
  int downX = 0;
  int downY = 0;
  const bool haveDown = mappedInput.wasScreenTouchDown(downX, downY);
  int tapX = 0;
  int tapY = 0;
  const bool haveTap = mappedInput.wasScreenTapped(tapX, tapY);

  if (haveDown || haveTap) {
    // Down moves the selector so the press is visible; Tap activates through
    // activateSelection(), so touch and the Confirm release cannot diverge.
    const auto moveSelector = [this](const int index) {
      if (selectorIndex != index) {
        selectorIndex = index;
        requestUpdate();
      }
    };
    const auto activate = [this](const int index) {
      selectorIndex = index;
      activateSelection();
    };
    // Theme indices are local to their band; selectorIndex counts the
    // recent-book tiles first unless the theme folds Continue Reading in.
    const auto menuSelector = [this, &metrics](const int row) {
      return metrics.homeContinueReadingInMenu ? row : row + static_cast<int>(recentBooks.size());
    };

    const int recentCount = std::min(static_cast<int>(recentBooks.size()), std::max(1, metrics.homeRecentBooksCount));
    const int renderedMenuCount =
        menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
    const Rect covers = coverRect();
    const Rect menu = menuRect();

    // Covers before menu, Down before Tap within each band — the order the
    // colTouch/rowTouch pair this replaced resolved in.
    int index = -1;
    if (haveDown && GUI.recentBookIndexFromPoint(renderer, covers, recentCount, downX, downY, index)) {
      moveSelector(index);
      return;
    }
    if (haveTap && GUI.recentBookIndexFromPoint(renderer, covers, recentCount, tapX, tapY, index)) {
      activate(index);
      return;
    }
    if (haveDown && GUI.menuIndexFromPoint(renderer, menu, renderedMenuCount, downX, downY, index)) {
      moveSelector(menuSelector(index));
      return;
    }
    if (haveTap && GUI.menuIndexFromPoint(renderer, menu, renderedMenuCount, tapX, tapY, index)) {
      activate(menuSelector(index));
      return;
    }
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
    activateSelection();
  }
}

Rect HomeActivity::coverRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight};
}

Rect HomeActivity::menuRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset,
              renderer.getScreenWidth(),
              renderer.getScreenHeight() - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                                            metrics.homeMenuTopOffset + metrics.buttonHintsHeight)};
}

// Open whatever selectorIndex currently points at: a recent-book cover tile
// below the count of recents, otherwise the menu row at that offset. Shared by
// the Confirm release and the touch paths, which must not diverge.
void HomeActivity::activateSelection() {
  if (selectorIndex < static_cast<int>(recentBooks.size())) {
    onSelectBook(recentBooks[selectorIndex].path);
    return;
  }
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

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  // Paint timing, split draw vs panel. A whole-screen render is normally dominated by the e-ink
  // refresh, so a single elapsed number would hide what we are actually chasing here: with a CJK
  // SD family selected, Han in book titles routes to the SD fallback font (SdCardFontSystem's
  // kUiFontSizes) and every glyph the 40-slot overflow ring cannot hold is an individual SD read.
  // logStats prints that as `miss=N (Nms)` per resident SD font; free/largest say whether a
  // batched prewarm could even be budgeted here (it needs largest/2 and 24KB of headroom --
  // SdCardFont.cpp MINI_FREE_FLOOR). Reset per paint so the numbers are for THIS selection move.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) fcm->resetStats();
  const unsigned long tStart = millis();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  const Rect coverBand = coverRect();
  coverRectX = coverBand.x;
  coverRectY = coverBand.y;
  coverRectW = coverBand.width;
  coverRectH = coverBand.height;

  GUI.drawRecentBookCover(renderer, coverBand, recentBooks, selectorIndex, coverRendered, coverBufferStored,
                          bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this));

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
      renderer, menuRect(), static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const unsigned long tDraw = millis();
  // A splashless wake with no restored frame leaves the sleep image on the panel and paints
  // nothing over it until this render. FAST is a differential update against a framebuffer
  // that no longer matches the glass, so the sleep image survives underneath Home; scrub it
  // once with HALF on the first paint only.
  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  const unsigned long displayMs = millis() - tDraw;

  LOG_DBG("HOME", "paint draw=%lu display=%lu free=%u largest=%u", tDraw - tStart, displayMs,
          static_cast<unsigned>(esp_get_free_heap_size()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  if (fcm) fcm->logStats("home");

  // Mirror the paint split to SD, for the same reason logDictPhase does
  // (DictionaryDefinitionActivity.cpp:188): the LOG_DBG above and fcm->logStats() only reach
  // a serial monitor, and the X3 is USB-locked — every report about this screen arrives as
  // opds_debug.txt and nothing else. The FRAG cover-pass line already lands there but is
  // emitted at the END of this function, so it timestamps the paint without describing it.
  //
  // miss/missMs is the discriminator for "Home is slow, but only after opening a Chinese
  // book". Han in a recent-book title routes to the SD fallback font, and the mini cache
  // that covers it is budgeted from free heap and largest block (SdCardFont MINI_FREE_FLOOR):
  // affordable on a fresh heap, unaffordable once the reader has fragmented it — at which
  // point every Han glyph becomes an individual SD read. Summed over EVERY resident SD font,
  // not one id, because the fallback is registered at several UI sizes; recents= is here
  // because Vega draws 4 titles to Lyra's 1, so the same per-glyph cost is paid 4x.
  {
    uint32_t misses = 0, missMs = 0;
    for (const auto& [fontId, font] : renderer.getSdCardFonts()) {
      if (!font) continue;
      misses += font->getStats().overflowMisses;
      missMs += font->getStats().overflowMissMs;
    }
    // snap= is the single most diagnostic bit on this line. 0 = fell through to the full
    // redraw (four SD cover bitmaps + both CJK title blocks, ~890ms); 1 = blitted back from
    // the RAM chunks (~10ms); 2 = streamed back from the SD file (~40-80ms). Every slow paint
    // should be snap=0, and a slow paint with snap=1 or 2 means the cause is elsewhere.
    SdDebugLog::setEnabled(true);
    SdDebugLog::log("HOME",
                    "paint draw=%lu display=%lu miss=%u missMs=%u snap=%d recents=%u sdFonts=%u free=%u largest=%u",
                    tDraw - tStart, displayMs, misses, missMs, bufferRestored ? (coverSdSnapshot ? 2 : 1) : 0,
                    static_cast<unsigned>(recentBooks.size()), static_cast<unsigned>(renderer.getSdCardFonts().size()),
                    static_cast<unsigned>(esp_get_free_heap_size()),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  }

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

  startActivityForResultNoThrow<ConfirmationActivity>(std::move(handler), renderer, mappedInput,
                                                      tr(STR_REMOVE_FROM_RECENTS), title);
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onReadingStatsOpen() { activityManager.goToReadingStats(); }
