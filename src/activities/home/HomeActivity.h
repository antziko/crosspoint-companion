#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasReadingStats = false;
  bool coverRendered = false;       // Track if cover has been rendered once
  bool coverBufferStored = false;   // Track if cover buffer is stored
  bool longPressFired = false;      // Swallow Confirm release after a long-press fired
  bool backLongPressFired = false;  // Swallow Back release after a hold-Back fired
  // Home can be entered while Back is still held (leaving Settings with Back):
  // ignore that stale release until a fresh Back press is seen (upstream #2619).
  bool backPressSeen = false;
  // Cover snapshot is stored in horizontal-strip chunks, not one contiguous
  // buffer: returning from the reader fragments the heap (free heap can be 60 KB+
  // while the largest contiguous block is < the ~33 KB the full region needs), so
  // a single malloc fails. Splitting into chunks fits the fragmented holes.
  //
  // Chunk sizing is only safe because the strip height is forced to a multiple of 8 (see
  // storeCoverBuffer). A logical row maps to one panel COLUMN after the portrait rotate, and
  // getRegionByteSize snaps each strip outward to byte boundaries, so a strip whose height is
  // not a multiple of 8 pays padding on both ends — PER STRIP. Two measured points from the
  // device, same 38016-byte region (X3, 528x576 logical = 66 panel rows x 528):
  //     144 rows -> 18 bytes x 528 = 9504 each,  4 chunks = 38016 allocated  (0 waste)
  //      31 rows ->  5 bytes x 528 = 2640 each, 19 chunks = 50160 allocated  (12144 waste, 32%)
  // The 2048/32 attempt hit the second row: it allocated 12 KB more than the region, left the
  // largest free block 8.7 KB shorter, and on failure walked to chunk 18/19 having driven free
  // heap to 5588 (minFreeEver=4508) once per paint. With -fno-exceptions that is a reboot
  // waiting to happen.
  //
  // With 8-row alignment the waste is exactly zero at ANY granularity, so the count can be
  // raised purely on fit grounds. 1 KB targets ~16-row strips (~1056 B) on X3 — nine times
  // smaller than the 9504 that failed at chunk 3/4 against largest=16372, at a cost of ~430
  // bytes of allocator headers.
  //
  // Any future change here must be justified against the measured `allocated=` field, never
  // against the region size.
  static constexpr size_t COVER_CHUNK_TARGET_BYTES = 1024;
  static constexpr int COVER_MAX_CHUNKS = 96;
  // Never start a snapshot that cannot finish. storeCoverBuffer is all-or-nothing, so a doomed
  // attempt allocates most of the heap, fails, and frees it — once per paint, with a free-heap
  // trough deep enough to threaten any allocation racing it. Skipping costs only what failing
  // already cost (the redraw happens either way) and removes the trough.
  static constexpr size_t COVER_SNAPSHOT_FREE_FLOOR = 8 * 1024;
  // ...and never take it out of an ALREADY fragmented heap, however much free heap there is.
  // The free-heap gate above only asks "can the 38 KB fit"; on a heap the reader has chopped up
  // it fits by filling ~36 small holes, and the long-lived allocations the rest of the paint
  // makes (glyph-cache misses on a CJK title, the SD cover reads) then land in whatever large
  // hole is left. Freeing the chunks at onExit() cannot undo that: the tile is gone but the
  // heap stays shredded for the whole session, and every network screen after Home runs under
  // its TLS gates.
  //
  // Measured on X3 (.pio/x3/opds_debug.txt), largest free block at Home entry -> largest at the
  // NEXT activity's entry, after the chunks were freed:
  //     61428 -> 57332   fresh boot, recovers fully
  //     38900 -> 38900   recovers fully
  //     18420 -> 12788   does NOT recover; that session's OPDS feed then truncated at 53049 of
  //                      130676 bytes with largest8=1396 during the handshake, and ended at a
  //                      permanent largest=6644 / "fetch aborted: low heap" on every retry
  //     14324 ->  6900   does not recover
  // 32 KB sits in the wide gap between the two groups. Below it the SD path is taken instead:
  // ~126 ms per paint against ~15 ms for RAM chunks, but still ~8x better than the ~1050 ms
  // full redraw, and it costs the heap a single ~528-byte buffer.
  static constexpr size_t COVER_SNAPSHOT_MIN_LARGEST_BLOCK = 32 * 1024;

  // SD-backed fallback. Finer chunks improve the odds of the RAM snapshot fitting but cannot
  // guarantee it — the heap after a CJK book is ~50 KB free in ~44 blocks whose largest is
  // 16372, and no chunk size makes an arbitrary block distribution work. This path does not
  // depend on the heap at all: the tile is streamed to a file one 8-row strip at a time
  // through a ~528-byte buffer, then streamed back on every later paint.
  //
  // ~40-80 ms per paint against the ~890 ms full redraw it replaces (four SD cover-bitmap
  // reads plus both CJK title blocks), so it is a large win even though it is much slower than
  // the ~10 ms RAM path. Written once per Home entry, so SD wear is negligible — and this is
  // the SD card, not SPIFFS, so the settings-write throttling rule does not apply.
  //
  // No staleness risk: HomeActivity is recreated on every entry with coverRendered=false, so
  // the file is always rewritten before it is ever read back, and reads are gated on a store
  // that succeeded in THIS instance.
  static constexpr int COVER_SD_STRIP_ROWS = 8;  // exactly 1 panel byte per column — zero padding
  static constexpr const char* COVER_SD_PATH = "/.crosspoint/home_tile.bin";
  bool coverSdSnapshot = false;  // true when the tile lives in COVER_SD_PATH, not in coverChunks
  bool storeCoverBufferToSd();
  bool restoreCoverBufferFromSd();
  uint8_t* coverChunks[COVER_MAX_CHUNKS] = {nullptr};
  size_t coverChunkSizes[COVER_MAX_CHUNKS] = {0};
  int coverChunkCount = 0;
  int coverChunkStripH = 0;  // logical rows per chunk (last chunk may be shorter)
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

  // Open whatever selectorIndex points at (recent cover tile or menu row);
  // shared by the Confirm release and the touch paths.
  void activateSelection();

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl, bool hasReadingStats) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::RECENTS) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
    if (item == HomeMenuItem::READING_STATS) return hasReadingStats ? i : 0;
    if (hasReadingStats) ++i;
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl, bool hasReadingStats) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::RECENTS;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
    if (hasReadingStats && idx == i++) return HomeMenuItem::READING_STATS;
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  // Long-press Confirm on a recent book: prompt to remove it from the recent list.
  void promptRemoveRecentBook(const std::string& path, const std::string& title);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onReadingStatsOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
