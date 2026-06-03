#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                SavedProgressPosition localKoPos, std::string localChapterName,
                                std::optional<uint16_t> currentParagraphIndex = std::nullopt)
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;

  // Bookmark sync summary, captured in syncBookmarks() for display on the result screen.
  bool bmSynced = false;     // True once a bookmark sync attempt completed (counts valid)
  int bmRemoteCount = 0;     // Bookmarks fetched from the server
  int bmLocalCount = 0;      // Local bookmarks before merge
  int bmMergedCount = 0;     // Total after union merge
  bool bmFetchOk = false;    // GET reached the server (OK or NOT_FOUND) — remote set is trustworthy
  bool bmUploadOk = false;   // PUT succeeded — local set actually propagated to the server

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // After a successful upload the result screen auto-returns to the reader so the
  // user doesn't have to press Back. millis() when UPLOAD_COMPLETE was entered;
  // Back still returns immediately.
  static constexpr unsigned long UPLOAD_COMPLETE_AUTO_RETURN_MS = 3000;
  unsigned long uploadCompleteAt = 0;

  // Guards returnToReader() so the level-triggered auto-return fires the reader switch once.
  bool returning = false;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  // Pull + union-merge + push bookmarks alongside progress. Silent (logs only);
  // never fails the progress sync. Requires `documentHash` already computed.
  void syncBookmarks();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
};
