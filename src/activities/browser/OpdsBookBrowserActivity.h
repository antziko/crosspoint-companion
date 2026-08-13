#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity, private UiAppHost {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  // Cached "book already on SD" flag per entry (1=on device, 0=not/navigation).
  // Computed once per feed load — NOT per render — so cursor moves don't re-stat
  // the SD card (~276 Storage.exists() calls/keypress before this cache). Parallel
  // to `entries`; refreshed by refreshDownloadedCache() whenever entries change.
  std::vector<uint8_t> downloadedCache;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  // Owned row labels: ListItem::label is a bare pointer, and the "already on
  // SD" marker prefixes the entry title, so the prefixed string needs a home
  // that outlives the build. Parallel to rowItems.
  std::vector<std::string> rowLabels;
  void rebuildRowItems();
  // Drop the feed vector (and its row buffers) to reclaim heap before TLS.
  void releaseEntries();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;
  // Set true (from the fetch progress callback, which polls Back) to abort an
  // in-flight feed download. Read/written only on the main task — fetchFeed and
  // its progress callback both run there — so no volatile/barrier needed.
  bool cancelFetch = false;
  bool lockLongPressBack = false;  // swallow BACK release after long-press-to-home
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // Viewport memory (top/visibleRows) for the browsing list; `selected` is
  // mirrored from selectorIndex at build/move time.
  freeink::ui::ListNav listNav;
  // Read by HttpDownloader between chunks; set by the Cancel button handler or
  // a Back press, both pumped from the download's progress callback.
  bool cancelDownload = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeAfterCancel = false;

  // Single screen fn dispatching on `state`: every state shares the themed
  // header and gets built through FreeInkUI.
  static void rootScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSearchEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiScreen& screen, bool withSearch);
  void buildBrowsingScreen(UiScreen& screen);
  void buildDownloadScreen(UiScreen& screen);
  void buildStatusScreen(UiScreen& screen);
  void activateSelected();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void refreshDownloadedCache();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  // Number of list rows that fit in the current orientation. Portrait (800 tall)
  // ~23; landscape (480 tall) ~12. Hardcoding caused rows to draw off-panel in
  // landscape, flooding drawPixel's per-pixel "Outside range" LOG_ERR.
  int itemsPerPage() const;
  bool preventAutoSleep() override { return true; }
};
