#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "SyncScope.h"
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
                                std::optional<uint16_t> currentParagraphIndex = std::nullopt,
                                bool sleepWhenDone = false, SyncScope scope = SyncScope::All)
      : Activity("KOReaderSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        localChapterName(std::move(localChapterName)),
        remoteProgress{},
        remotePosition{},
        localProgress(std::move(localKoPos)),
        sleepWhenDone(sleepWhenDone),
        syncScope(scope) {}

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
    FEATURE_DONE,  // single-feature scope finished: show summary, auto-return
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  // Which feature(s) this run syncs. ALL keeps the full progress-comparison flow;
  // the single-feature scopes skip progress and end on the FEATURE_DONE summary.
  SyncScope syncScope = SyncScope::All;

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

  // Phased status display: feature index ([n/total]) + a sub-phase label, so a long
  // leg shows motion instead of one frozen "Syncing X" string. syncStepTotal is set
  // once in performSync (3 for ALL, 1 for single-feature); the prefix is omitted when
  // total <= 1. See setSyncPhase().
  int syncStepIndex = 0;
  int syncStepTotal = 0;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;

  // Bookmark sync summary, captured in syncBookmarks() for display on the result screen.
  bool bmSynced = false;    // True once a bookmark sync attempt completed (counts valid)
  int bmRemoteCount = 0;    // Bookmarks fetched from the server
  int bmLocalCount = 0;     // Local bookmarks before merge
  int bmMergedCount = 0;    // Total after union merge
  bool bmFetchOk = false;   // GET reached the server (OK or NOT_FOUND) — remote set is trustworthy
  bool bmUploadOk = false;  // PUT succeeded — local set actually propagated to the server

  // Reading-stats sync summary, captured in syncStats() for the result screen.
  bool statsSynced = false;           // True once a stats sync attempt completed
  bool statsFetchOk = false;          // GET reached the server (OK or NOT_FOUND)
  bool statsUploadOk = false;         // PUT of this device's counter succeeded
  uint32_t statsTotalAllDevices = 0;  // local counter + sum of other devices' counters
  bool dictSynced = false;            // True once a dictionary-history merge attempt ran
  bool dictSkippedLowHeap = false;    // True if dict sync was skipped for low free heap
  int dictMergedWords = 0;            // remote adds merged in from other devices
  int dictDeletedWords = 0;           // remote deletes applied from other devices
  int dictUploadedWords = 0;          // our own history entries uploaded in "dh"
  int dictUploadedDeletes = 0;        // our own tombstones (deletes) uploaded in "dh"
  bool fcSynced = false;              // True once a flashcard merge attempt ran
  bool fcSkippedLowHeap = false;      // True if flashcard sync was skipped for low free heap
  int fcMergedCards = 0;              // remote cards merged in from other devices
  int fcDeletedCards = 0;             // remote deletes applied from other devices
  int fcUploadedCards = 0;            // phase-2 delta cards uploaded in "fc" (new/changed)
  int fcHealCards = 0;                // phase-3 rolling-slice cards uploaded (re-broadcast heal)
  int fcUploadedDeletes = 0;          // our own tombstones (deletes) uploaded in "fc"
  int fcDeckCount = 0;                // local deck size (backfill denominator)
  int fcCursor = 0;                   // rolling-cursor position after this sync (backfill numerator)
  // Server build clue for the page header: tag echoed by the stats PUT
  // ("stats-v1"), "no stats" when the endpoint 404'd, empty while unknown.
  char serverTag[32] = {0};

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // After a successful upload the result screen auto-returns to the reader so the
  // user doesn't have to press Back. millis() when UPLOAD_COMPLETE was entered;
  // Back still returns immediately.
  static constexpr unsigned long UPLOAD_COMPLETE_AUTO_RETURN_MS = 3000;
  unsigned long uploadCompleteAt = 0;

  // Guards returnToReader() so the level-triggered auto-return fires the reader switch once.
  bool returning = false;

  // When true, this sync was launched from the reader's "sync before sleep" flow: on a
  // successful completion the device deep-sleeps instead of returning to the reader.
  bool sleepWhenDone = false;
  // Set on a successful sync completion (progress uploaded, or remote applied). Gates the
  // sleepWhenDone behaviour so a failure / Back-out returns to the reader awake as usual.
  bool syncSucceeded = false;

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
  // Uses NO keep-alive session: the ~2.7KB upload body needs an unfragmented arena,
  // so GET and PUT run as separate fresh connections (see the .cpp for the why).
  void syncBookmarks();
  // Pull + merge + push per-device reading-time counters alongside progress.
  // Same contract as syncBookmarks(): silent, never fails the progress sync.
  // `includeDict` folds/uploads the per-book dictionary history ("dh");
  // `includeFlashcards` folds/uploads the per-book flashcard deck ("fc");
  // `includeGlobal` runs the all-books global-counter round-trips. The per-book
  // counter merge always runs (it shares the same GET/PUT). For DICT/FLASHCARDS
  // scope, pass includeGlobal=false to skip the extra global legs. Opens its own
  // keep-alive connection.
  void syncStats(bool includeDict, bool includeGlobal, bool includeFlashcards);
  // Render the shared "Also synced" footer (bookmarks/dict/stats summary) starting at
  // `y`, advancing and returning the new cursor. Used by SHOWING_RESULT, NO_REMOTE_PROGRESS
  // and FEATURE_DONE so the summary layout lives in one place.
  int drawAlsoSyncedFooter(int sideX, int y, int lhFoot, bool showAlsoLabel = true);
  // Render "[i/total] phase" into statusMessage (prefix omitted when total <= 1) and
  // block until the paint completes, so the message is visible during the blocking
  // network leg that follows. Sets state = SYNCING.
  void setSyncPhase(const char* phase);
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
};
