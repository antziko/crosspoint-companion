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
  // Cover snapshot is stored in horizontal-strip chunks, not one contiguous
  // buffer: returning from the reader fragments the heap (free heap can be 60 KB+
  // while the largest contiguous block is < the ~33 KB the full region needs), so
  // a single malloc fails. Splitting into ~12 KB chunks fits the fragmented holes.
  static constexpr size_t COVER_CHUNK_TARGET_BYTES = 12000;
  static constexpr int COVER_MAX_CHUNKS = 16;  // 16 * 12KB = 192KB region ceiling, far above need
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
};
