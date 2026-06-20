#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "CrossPointState.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "GlobalReadingStats.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/LookupHistory.h"

namespace {
void syncTimeWithNTP() {
  // Stop SNTP if already running (can't reconfigure while running)
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }

  // Configure SNTP
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  // Wait for time to sync (with timeout)
  int retry = 0;
  const int maxRetries = 50;  // 5 seconds max
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < maxRetries) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    retry++;
  }

  if (retry < maxRetries) {
    LOG_DBG("KOSync", "NTP time synced");
  } else {
    LOG_DBG("KOSync", "NTP sync timeout, using fallback");
  }
}
}  // namespace

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("KOSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("KOSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void KOReaderSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  syncSucceeded = true;  // remote applied: a sleepWhenDone sync may now deep-sleep
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() {
  // The auto-return check in loop() is level-triggered, so guard against re-entry: fire the
  // exit exactly once.
  if (returning) return;
  returning = true;

  // On a successful sync, advance the "last synced" marker (reading seconds at sync time) so the
  // reader's "sync before sleep" prompt only re-triggers after more reading accrues. Done here at
  // the single success funnel — not gated on the stats endpoint — so it also covers servers
  // without reading-stats support, and applies to both menu-initiated and sleep syncs.
  if (syncSucceeded) {
    const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(epubPath));
    BookReadingStats stats = BookReadingStats::load(cachePath);
    if (stats.lastSyncReadingSeconds != stats.totalReadingSeconds) {
      stats.lastSyncReadingSeconds = stats.totalReadingSeconds;
      stats.save(cachePath);
    }
  }

  // "Sync before sleep" flow: on a successful completion, deep-sleep instead of rebuilding the
  // reader. Defer to the main loop (it owns enterDeepSleep) and stay the current activity so its
  // onExit() runs as part of the sleep teardown — which also powers WiFi down and resets the chip,
  // so no silent restart is needed (see onExit()).
  if (sleepWhenDone && syncSucceeded) {
    APP_STATE.requestManualSleep = true;
    return;
  }
  activityManager.goToReader(epubPath);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  // Hand the 32KB inflate window back to the heap for the TLS handshakes below.
  // This activity never builds sections (so it never needs the window), the epub
  // was already released by EpubReaderActivity, and onExit() always reboots — which
  // re-reserves the window on a fresh heap — so it is never re-allocated here.
  InflateReader::releaseWindow();
  LOG_DBG("KOSync", "Released inflate window for TLS (heap: %u)", (unsigned)ESP.getFreeHeap());

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Sync time with NTP before making API requests
  syncTimeWithNTP();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void KOReaderSyncActivity::performSync() {
  // Calculate document hash based on user's preferred method
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
  } else {
    documentHash = KOReaderDocumentId::calculate(epubPath);
  }
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("KOSync", "Document hash: %s", documentHash.c_str());

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Fetch remote progress
  const auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  // Sync bookmarks and reading stats alongside progress whenever the server is
  // reachable (OK or NOT_FOUND). Silent and best-effort: neither changes the
  // progress sync outcome below.
  if (result == KOReaderSyncClient::OK || result == KOReaderSyncClient::NOT_FOUND) {
    syncBookmarks();
    syncStats();
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    // No remote progress - offer to upload
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  hasRemoteProgress = true;
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
  remotePosition = ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // Release epub before the TLS handshake to free ~30KB RAM. localProgress was
  // pre-computed before the Epub was released, so this is safe.
  epub.reset();

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
    uploadCompleteAt = millis();  // start the auto-return countdown
    syncSucceeded = true;         // upload landed: a sleepWhenDone sync may now deep-sleep
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::syncBookmarks() {
  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_BOOKMARKS);
  }
  requestUpdateAndWait();

  // Get title/author from epub, then release it before TLS calls to free ~30KB RAM
  // for the handshake. epubPath is already a member so loadForBook doesn't need epub live.
  // If epub fails to load, proceed with empty strings — the bookmark file is keyed by
  // the path CRC, not by title/author (those are display metadata only).
  ensureEpubLoaded();
  std::string bookTitle;
  std::string bookAuthor;
  if (epub) {
    bookTitle = epub->getTitle();
    bookAuthor = epub->getAuthor();
    epub.reset();  // Release before TLS calls; performSync reloads after syncBookmarks returns
  }

  // The reader unloaded its bookmarks when it exited; reload from disk for this book.
  if (!BOOKMARKS.loadForBook(epubPath, bookTitle, bookAuthor, "epub")) {
    LOG_ERR("KOSync", "Skipping bookmark sync: failed to load local bookmarks");
    return;
  }
  // Count only syncable bookmarks — device-only "return here" marks are never pushed,
  // so excluding them keeps the summary consistent with what actually syncs.
  const auto countSyncable = [] {
    const auto& bms = BOOKMARKS.getBookmarks();
    return static_cast<int>(
        std::count_if(bms.begin(), bms.end(), [](const struct Bookmark& b) { return !b.returnMark; }));
  };
  bmLocalCount = countSyncable();

  // Pull remote, reconcile with local (union bookmarks, propagate tombstoned deletes).
  std::string remoteJson;
  const auto getResult = KOReaderSyncClient::getBookmarks(documentHash, remoteJson);
  if (getResult == KOReaderSyncClient::OK) {
    // Elaborated type: BaseTheme.h's UIIcon enum has a 'Bookmark' enumerator that
    // otherwise hides the struct in this translation unit.
    std::vector<struct Bookmark> remoteBms;
    std::vector<Tombstone> remoteTombs;
    if (BookmarkStore::parseFromJson(remoteJson.c_str(), remoteBms, remoteTombs)) {
      bmRemoteCount = static_cast<int>(remoteBms.size());
      const size_t added = BOOKMARKS.mergeFrom(remoteBms, remoteTombs);  // self-persists
      LOG_DBG("KOSync", "Merged %u remote bookmark(s)", (unsigned)added);
    }
  } else if (getResult != KOReaderSyncClient::NOT_FOUND) {
    // Fetch failed, but still upload local set so the server learns our bookmarks.
    LOG_ERR("KOSync", "Bookmark fetch failed: %s", KOReaderSyncClient::errorString(getResult));
  }
  // NOT_FOUND just means the server has nothing stored yet — the fetch itself succeeded.
  bmFetchOk = (getResult == KOReaderSyncClient::OK || getResult == KOReaderSyncClient::NOT_FOUND);

  bmMergedCount = countSyncable();
  bmSynced = true;

  // Push the reconciled set + tombstones so other devices converge on next sync.
  const std::string localJson = BookmarkStore::serializeToJson(BOOKMARKS.getBookmarks(), BOOKMARKS.getTombstones());
  const auto putResult = KOReaderSyncClient::updateBookmarks(documentHash, localJson);
  bmUploadOk = (putResult == KOReaderSyncClient::OK);
  if (!bmUploadOk) {
    // A failed upload means local deletes/additions never reached the server, so other
    // devices won't converge. Surface this on the result screen rather than hiding it.
    LOG_ERR("KOSync", "Bookmark upload failed: %s", KOReaderSyncClient::errorString(putResult));
  }
}

void KOReaderSyncActivity::syncStats() {
  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_STATS);
  }
  requestUpdateAndWait();

  // stats.bin lives in the book's path-hash cache dir (same derivation as the Epub ctor).
  const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(epubPath));
  BookReadingStats stats = BookReadingStats::load(cachePath);
  const uint32_t prevLocalSeconds = stats.totalReadingSeconds;
  const uint32_t prevRemoteSeconds = stats.remoteOtherSeconds;

  // Per-book dictionary-history cross-device merge (Lamport-versioned). Serialize
  // OUR history pre-merge for upload; the fold below merges every OTHER device's
  // "dh" blob into the local history during the stats GET. Gated on free heap as a
  // backstop only: the serialize/decode buffers are nothrow (null -> skipped) and
  // the response buffer is hard-capped, so this just avoids attempting when heap is
  // genuinely too low. Kept well below the typical reader-context free heap so it
  // doesn't spuriously skip on the constrained X3/X4.
  constexpr uint32_t kDictSyncMinHeap = 48 * 1024;
  constexpr size_t kDictBlobCap = 4096;
  const bool doDictSync = ESP.getFreeHeap() > kDictSyncMinHeap;
  std::unique_ptr<uint8_t[]> dictUp;
  size_t dictUpLen = 0;
  struct DictMergeCtx {
    const std::string* cachePath;
    int merged;   // remote adds applied
    int deleted;  // remote deletes applied
  } dictMergeCtx{&cachePath, 0, 0};
  StatsDatedFold dictFold;
  if (doDictSync) {
    dictUp = makeUniqueNoThrow<uint8_t[]>(kDictBlobCap);
    if (dictUp)
      dictUpLen =
          LookupHistory::serializeBlob(cachePath, dictUp.get(), kDictBlobCap, &dictUploadedWords, &dictUploadedDeletes);
    dictFold.ctx = &dictMergeCtx;
    dictFold.fn = [](void* ctx, const uint8_t* blob, size_t len) {
      auto* c = static_cast<DictMergeCtx*>(ctx);
      int del = 0;
      c->merged += LookupHistory::mergeBlob(*c->cachePath, blob, len, &del);
      c->deleted += del;
    };
  } else {
    LOG_DBG("KOSync", "Low heap (%u); skipping dict history sync", (unsigned)ESP.getFreeHeap());
  }

  // Heap, not stack: 8 entries is ~290 bytes — over the 256-byte stack-local
  // guideline. Reused below for the global-counter phase.
  auto entriesBuf = makeUniqueNoThrow<KOReaderStatsEntry[]>(KOReaderSyncClient::MAX_STATS_DEVICES);
  if (!entriesBuf) {
    LOG_ERR("KOSync", "OOM: stats entries");
    return;
  }
  KOReaderStatsEntry* entries = entriesBuf.get();

  // Pull every device's counter. NOT_FOUND = server has nothing yet; still upload ours.
  // The dict fold (when enabled) merges other devices' lookup history during this GET.
  size_t count = 0;
  const auto getResult =
      KOReaderSyncClient::getStats(documentHash, entries, count, nullptr, doDictSync ? &dictFold : nullptr);
  statsFetchOk = (getResult == KOReaderSyncClient::OK || getResult == KOReaderSyncClient::NOT_FOUND);
  if (statsFetchOk) {
    uint32_t othersSeconds = 0;
    uint32_t remoteDay = 0;
    uint8_t remoteHour = 0;
    uint8_t remoteMinute = 0;
    for (size_t i = 0; i < count; i++) {
      const KOReaderStatsEntry& e = entries[i];
      if (strcmp(e.deviceId, KOReaderSyncClient::deviceId()) == 0) {
        // Self-heal: if the server's copy of OUR counter is ahead of the local one,
        // stats.bin was lost (cache wipe, book moved before the relocation fix) —
        // adopt the server value as a floor so the upload below can't clobber it.
        if (e.seconds > stats.totalReadingSeconds) {
          LOG_INF("KOSync", "Stats self-heal: local %lus -> server %lus",
                  static_cast<unsigned long>(stats.totalReadingSeconds), static_cast<unsigned long>(e.seconds));
          stats.totalReadingSeconds = e.seconds;
        }
        if (e.lastReadDayIndex > stats.lastReadDayIndex) {
          stats.lastReadDayIndex = e.lastReadDayIndex;
          stats.lastReadHour = e.lastReadHour;
          stats.lastReadMinute = e.lastReadMinute;
        }
      } else {
        othersSeconds += e.seconds;
        if (e.lastReadDayIndex > remoteDay) {
          remoteDay = e.lastReadDayIndex;
          remoteHour = e.lastReadHour;
          remoteMinute = e.lastReadMinute;
        }
      }
    }
    stats.remoteOtherSeconds = othersSeconds;
    stats.remoteLastReadDayIndex = remoteDay;
    stats.remoteLastReadHour = remoteHour;
    stats.remoteLastReadMinute = remoteMinute;
  } else {
    LOG_ERR("KOSync", "Stats fetch failed: %s", KOReaderSyncClient::errorString(getResult));
  }

  // Push this device's counter (monotonic; replaces only our hash field on the server).
  KOReaderStatsEntry mine;
  mine.seconds = stats.totalReadingSeconds;
  mine.lastReadDayIndex = stats.lastReadDayIndex;
  mine.lastReadHour = stats.lastReadHour;
  mine.lastReadMinute = stats.lastReadMinute;
  // Upload our scalar counters + our (pre-merge) dictionary-history blob in "dh".
  const auto putResult =
      KOReaderSyncClient::updateStats(documentHash, mine, nullptr, 0, dictUp ? dictUp.get() : nullptr, dictUpLen);
  statsUploadOk = (putResult == KOReaderSyncClient::OK);
  if (!statsUploadOk) {
    LOG_ERR("KOSync", "Stats upload failed: %s", KOReaderSyncClient::errorString(putResult));
  }

  // Server build clue for the page header: tag echoed by a stats-enabled server,
  // "stats" if the PUT succeeded against a tag-less stats build, "no stats" if the
  // endpoint doesn't exist (404: stock/legacy server). Transport errors leave it
  // empty — server build unknown.
  if (statsUploadOk) {
    const char* tag = KOReaderSyncClient::statsServerTag();
    snprintf(serverTag, sizeof(serverTag), "%s", tag[0] != '\0' ? tag : "stats");
  } else if (putResult == KOReaderSyncClient::SERVER_ERROR && KOReaderSyncClient::lastHttpCode == 404) {
    snprintf(serverTag, sizeof(serverTag), "%s", tr(STR_SYNC_SERVER_NO_STATS));
  }

  // Persist only on change (SD write throttling): remote sum updated or self-heal fired.
  // NB: the "sync before sleep" marker (lastSyncReadingSeconds) is advanced separately in
  // returnToReader() on overall sync success, so it covers servers without stats support too.
  if (stats.totalReadingSeconds != prevLocalSeconds || stats.remoteOtherSeconds != prevRemoteSeconds) {
    stats.save(cachePath);
  }
  statsTotalAllDevices = stats.displayTotalSeconds();
  statsSynced = true;

  // Dictionary-history merge result (surfaced in the "Also synced" footer). Report
  // whenever we attempted it (even +0, so there's confirmation it ran); flag the
  // low-heap skip distinctly so a missing line is never silent.
  dictSynced = doDictSync;
  dictSkippedLowHeap = !doDictSync;
  dictMergedWords = dictMergeCtx.merged;
  dictDeletedWords = dictMergeCtx.deleted;

  SdDebugLog::log("KOSync", "stats sync: local=%lu others=%lu fetch=%d upload=%d dictUp=%d/%d dictMerged=%d dictDel=%d",
                  static_cast<unsigned long>(stats.totalReadingSeconds),
                  static_cast<unsigned long>(stats.remoteOtherSeconds), statsFetchOk ? 1 : 0, statsUploadOk ? 1 : 0,
                  dictUploadedWords, dictUploadedDeletes, dictMergedWords, dictDeletedWords);

  // --- Global (all-books) counter, same per-device scheme under a reserved
  // pseudo-document. The name can't collide with real documents: binary-mode
  // hashes are 32 hex chars and filename-mode hashes are MD5 hex too.
  if (statsUploadOk) {  // skip the extra round-trips when the server has no stats support
    // Underscores, not hyphens: the sync server's gin router compiles the
    // GET /syncs/stats/:document param to a \w+ pattern, so a hyphenated doc name
    // 404s on fetch (PUT has no path param and would still store it) — that broke
    // cross-device global merge entirely. \w allows [A-Za-z0-9_].
    const std::string globalDoc = "crosspoint_global_stats";
    auto global = makeUniqueNoThrow<GlobalReadingStats>();
    if (!global) {
      LOG_ERR("KOSync", "OOM: global stats");
      return;
    }
    GlobalReadingStats::load(*global);
    const uint32_t prevGlobalLocal = global->totalReadingSeconds;
    const uint32_t prevGlobalRemote = global->remoteOtherSeconds;

    // Cross-device dated-history merge (global only): fold every OTHER device's
    // dated blob into an accumulator that becomes the remote snapshot. cap+stream
    // — one reusable scratch per device, freed by the client between folds; here
    // we hold just the accumulator + one scratch (~880 B each, heap).
    auto remoteAccum = makeUniqueNoThrow<ReadingTimeHistory>();
    auto foldScratch = makeUniqueNoThrow<ReadingTimeHistory>();
    struct FoldCtx {
      ReadingTimeHistory* accum;
      ReadingTimeHistory* scratch;
      bool any;
    } foldCtx{remoteAccum.get(), foldScratch.get(), false};
    StatsDatedFold fold;
    if (remoteAccum && foldScratch) {
      fold.ctx = &foldCtx;
      fold.fn = [](void* ctx, const uint8_t* blob, size_t len) {
        auto* c = static_cast<FoldCtx*>(ctx);
        if (c->scratch->deserializeBlob(blob, len)) {
          c->accum->mergeFrom(*c->scratch);
          c->any = true;
        }
      };
    } else {
      LOG_ERR("KOSync", "OOM: dated merge buffers");  // scalar still proceeds below
    }

    count = 0;
    const auto gGet = KOReaderSyncClient::getStats(globalDoc, entries, count, fold.fn ? &fold : nullptr);
    if (gGet == KOReaderSyncClient::OK || gGet == KOReaderSyncClient::NOT_FOUND) {
      uint32_t othersSeconds = 0;
      for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].deviceId, KOReaderSyncClient::deviceId()) == 0) {
          // Same self-heal floor as the per-book counter.
          if (entries[i].seconds > global->totalReadingSeconds) {
            LOG_INF("KOSync", "Global stats self-heal: local %lus -> server %lus",
                    static_cast<unsigned long>(global->totalReadingSeconds),
                    static_cast<unsigned long>(entries[i].seconds));
            global->totalReadingSeconds = entries[i].seconds;
          }
        } else {
          othersSeconds += entries[i].seconds;
        }
      }
      global->remoteOtherSeconds = othersSeconds;
      // Adopt the folded snapshot of OTHER devices' dated history. On a clean
      // OK/NOT_FOUND with no other devices this is empty — correct (no remote
      // data). Skipped on fetch error so the last good snapshot is preserved.
      if (fold.fn) global->remoteHistory = *remoteAccum;
    } else {
      LOG_ERR("KOSync", "Global stats fetch failed: %s", KOReaderSyncClient::errorString(gGet));
    }

    // Upload OUR local dated history alongside the global counter (base64 "h").
    auto datedBuf = makeUniqueNoThrow<uint8_t[]>(ReadingTimeHistory::BLOB_MAX_BYTES);
    size_t datedLen = 0;
    if (datedBuf) datedLen = global->history.serializeBlob(datedBuf.get(), ReadingTimeHistory::BLOB_MAX_BYTES);

    KOReaderStatsEntry gMine;  // lastRead fields stay 0 — meaningless for the global counter
    gMine.seconds = global->totalReadingSeconds;
    const auto gPut = KOReaderSyncClient::updateStats(globalDoc, gMine, datedBuf ? datedBuf.get() : nullptr, datedLen);
    if (gPut != KOReaderSyncClient::OK) {
      LOG_ERR("KOSync", "Global stats upload failed: %s", KOReaderSyncClient::errorString(gPut));
    }

    // Persist on change. Manual sync is user-initiated (not per-page), so writing
    // when a remote snapshot was folded is within the SD-throttle policy.
    if (global->totalReadingSeconds != prevGlobalLocal || global->remoteOtherSeconds != prevGlobalRemote ||
        foldCtx.any) {
      global->save();
    }
    SdDebugLog::log("KOSync", "global stats sync: local=%lu others=%lu dated=%d put=%d",
                    static_cast<unsigned long>(global->totalReadingSeconds),
                    static_cast<unsigned long>(global->remoteOtherSeconds), foldCtx.any ? 1 : 0,
                    gPut == KOReaderSyncClient::OK ? 1 : 0);
  }
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();

  // X3 HTTPS troubleshooting: enable the SD trace for the lifetime of this
  // activity (covers KOReaderSyncClient's authenticate/getProgress/updateProgress/
  // getBookmarks/updateBookmarks calls). See SdDebugLog.h / SUMMARY.md Part B Appendix.
  SdDebugLog::setEnabled(true);

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_DBG("KOSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  SdDebugLog::setEnabled(false);

  // Sleeping after a successful sleepWhenDone sync: skip the silent restart. enterDeepSleep()
  // (driven by APP_STATE.requestManualSleep in the main loop) tears WiFi down and a deep-sleep
  // wake is a full chip reset, so the heap-defrag reboot would only fight the sleep gesture.
  if (sleepWhenDone && syncSucceeded) {
    return;
  }

  if (wifiActivated) {
    // silentRestartToReader() powers the modem fully down (WIFI_OFF) before the soft
    // reset — leaving the radio on across ESP.restart() hangs X4's reader-boot. No need
    // to disconnect here first.
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const auto* activeServer = KOREADER_STORE.getServer(static_cast<size_t>(KOREADER_STORE.getActiveIndex()));
  // Header is just the active server name (fall back to the generic title when the
  // server has no name), plus the optional stats tag below.
  char syncHeader[96];
  if (activeServer && !activeServer->name.empty()) {
    snprintf(syncHeader, sizeof(syncHeader), "%s", activeServer->name.c_str());
  } else {
    snprintf(syncHeader, sizeof(syncHeader), "%s", tr(STR_KOREADER_SYNC));
  }
  // Server build clue, learned from the stats PUT during this sync: the tag the
  // stats-enabled server echoed (e.g. "stats-v1"), or "no stats" when the PUT
  // 404'd (stock/legacy server without the extension). Empty until known.
  if (serverTag[0] != '\0') {
    const size_t len = strlen(syncHeader);
    snprintf(syncHeader + len, sizeof(syncHeader) - len, " [%s]", serverTag);
  }
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 syncHeader);

  int top = screen.y + screen.height / 2 - 40;
  if (state == NO_CREDENTIALS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), true,
                              EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_KOREADER_SETUP_HINT), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    const int sideX = screen.x + metrics.contentSidePadding;
    const int contentW = screen.width - 2 * metrics.contentSidePadding;

    // Vertical rhythm — one place to tune, no scattered top+NN literals. A running
    // `y` cursor advances by these named steps instead of hardcoded offsets.
    const int lhData = renderer.getLineHeight(UI_10_FONT_ID);
    const int lhFoot = renderer.getLineHeight(UI_10_FONT_ID);
    const int DATA_ROW = lhData + 3;   // step between data lines in a card
    const int LABEL_ROW = lhData + 2;  // card label line (Remote:/Local:, now UI_10 bold)
    const int OPTION_H = lhData + 10;  // selectable row (highlight bar height)
    const int CARD_GAP = 10;           // between remote card and local card
    const int detailX = sideX + 12;    // chapter/page indent under the card label
    const int SECTION_GAP = 10;        // generic block gap

    int y = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

    // Chapter names: remote needs the live Epub (loaded lazily in performSync before
    // this state); local was pre-computed before the Epub was released.
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    char buf[128];

    // Choice rows render as buttons so they read as interactive (not body text):
    // selected = filled black box + inverted text, unselected = outlined box.
    const auto drawOption = [&](int by, const char* label, bool selected) {
      constexpr int cr = 6;
      if (selected) {
        renderer.fillRoundedRect(sideX, by, contentW, OPTION_H, cr, Color::Black);
      } else {
        renderer.drawRoundedRect(sideX, by, contentW, OPTION_H, 1, cr, true);
      }
      const int ty = by + (OPTION_H - lhData) / 2;
      renderer.drawText(UI_10_FONT_ID, detailX, ty, label, !selected);
    };

    // --- REMOTE card ---
    // Label + source device on one line. Prefer the unique efuse id (already parsed
    // into deviceId) so two CrossPoint devices are distinguishable; the generic
    // "device" name is identical for every CrossPoint upload.
    char remoteLabel[80];
    if (!remoteProgress.deviceId.empty()) {
      const std::string& id = remoteProgress.deviceId;
      const char* tail = id.size() >= 4 ? id.c_str() + id.size() - 4 : id.c_str();  // short tag
      snprintf(remoteLabel, sizeof(remoteLabel), "%s  (%s:%s)", tr(STR_REMOTE_LABEL),
               remoteProgress.device.empty() ? "device" : remoteProgress.device.c_str(), tail);
    } else if (!remoteProgress.device.empty()) {
      snprintf(remoteLabel, sizeof(remoteLabel), "%s  (%s)", tr(STR_REMOTE_LABEL), remoteProgress.device.c_str());
    } else {
      snprintf(remoteLabel, sizeof(remoteLabel), "%s", tr(STR_REMOTE_LABEL));
    }
    renderer.drawText(UI_10_FONT_ID, sideX, y, remoteLabel, true, EpdFontFamily::BOLD);
    y += LABEL_ROW;
    renderer.drawText(UI_10_FONT_ID, detailX, y, remoteChapter.c_str());
    y += DATA_ROW;
    snprintf(buf, sizeof(buf), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, detailX, y, buf);
    y += DATA_ROW + 2;

    // Choice 0 lives in the card it acts on.
    drawOption(y, tr(STR_APPLY_REMOTE), selectedOption == 0);
    y += OPTION_H + CARD_GAP;

    // --- LOCAL card ---
    renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_LOCAL_LABEL), true, EpdFontFamily::BOLD);
    y += LABEL_ROW;
    renderer.drawText(UI_10_FONT_ID, detailX, y, localChapter.c_str());
    y += DATA_ROW;
    snprintf(buf, sizeof(buf), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, detailX, y, buf);
    y += DATA_ROW + 2;

    drawOption(y, tr(STR_UPLOAD_LOCAL), selectedOption == 1);
    y += OPTION_H + SECTION_GAP;

    // --- "Also synced" footer: bookmarks + reading stats always merge, so they are
    // passive info, not a choice. Extra gap above separates it from the choice buttons.
    if (bmSynced || statsSynced || dictSynced || dictSkippedLowHeap) {
      y += SECTION_GAP;
      renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_ALSO_SYNCED), true, EpdFontFamily::BOLD);
      y += lhFoot + 2;
      if (bmSynced) {
        // Two lines: "Bookmarks" + counts, then indented fetch/upload status. Split
        // because one combined line overflows the width, worse when "failed" replaces "ok".
        char bmCounts[96];
        snprintf(bmCounts, sizeof(bmCounts), tr(STR_BOOKMARK_DIFF_FORMAT), bmRemoteCount, bmLocalCount, bmMergedCount);
        snprintf(buf, sizeof(buf), "%s  %s", tr(STR_BOOKMARKS), bmCounts);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
        char bmStatusStr[64];
        snprintf(bmStatusStr, sizeof(bmStatusStr), tr(STR_BOOKMARK_SYNC_STATUS_FORMAT),
                 bmFetchOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER),
                 bmUploadOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER));
        snprintf(buf, sizeof(buf), "  %s", bmStatusStr);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
      }
      if (dictSynced) {
        if (bmSynced) y += 6;  // separate from the bookmark block above
        snprintf(buf, sizeof(buf), tr(STR_SYNC_DICT_FORMAT), dictUploadedWords, dictUploadedDeletes, dictMergedWords,
                 dictDeletedWords);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
        // Dict rides the stats GET/PUT, so its fetch/upload status is the stats one.
        char dStatus[64];
        snprintf(dStatus, sizeof(dStatus), tr(STR_BOOKMARK_SYNC_STATUS_FORMAT),
                 statsFetchOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER),
                 statsUploadOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER));
        snprintf(buf, sizeof(buf), "  %s", dStatus);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
      } else if (dictSkippedLowHeap) {
        if (bmSynced) y += 6;
        renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_SYNC_DICT_SKIPPED));
        y += lhFoot + 2;
      }
      if (statsSynced) {
        if (bmSynced || dictSynced || dictSkippedLowHeap) y += 6;  // reading time sits at the bottom
        char durBuf[24];
        BookReadingStats::formatDuration(statsTotalAllDevices, durBuf, sizeof(durBuf));
        snprintf(buf, sizeof(buf), tr(STR_STATS_ALL_DEVICES_FORMAT), durBuf);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
      }
    }

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    const int sideX = screen.x + metrics.contentSidePadding;
    const int lhFoot = renderer.getLineHeight(UI_10_FONT_ID);
    const int LABEL_ROW = renderer.getLineHeight(UI_12_FONT_ID) + 2;
    const int SECTION_GAP = 10;

    // Centered prompt (UI_12 bold title to match SHOWING_RESULT's hierarchy).
    int y = top;
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID, y, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    y += LABEL_ROW;
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, y, tr(STR_UPLOAD_PROMPT));
    y += renderer.getLineHeight(UI_10_FONT_ID) + SECTION_GAP;

    // Same "Also synced" footer as SHOWING_RESULT: passive info, extra gap above.
    if (bmSynced || statsSynced || dictSynced || dictSkippedLowHeap) {
      y += SECTION_GAP;
      renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_ALSO_SYNCED), true, EpdFontFamily::BOLD);
      y += lhFoot + 2;
      char buf[128];
      if (bmSynced) {
        // Two lines: "Bookmarks" + counts, then indented fetch/upload status (one line overflows).
        char bmCounts[96];
        snprintf(bmCounts, sizeof(bmCounts), tr(STR_BOOKMARK_DIFF_FORMAT), bmRemoteCount, bmLocalCount, bmMergedCount);
        snprintf(buf, sizeof(buf), "%s  %s", tr(STR_BOOKMARKS), bmCounts);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
        char bmStatusStr[64];
        snprintf(bmStatusStr, sizeof(bmStatusStr), tr(STR_BOOKMARK_SYNC_STATUS_FORMAT),
                 bmFetchOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER),
                 bmUploadOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER));
        snprintf(buf, sizeof(buf), "  %s", bmStatusStr);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
      }
      if (dictSynced) {
        if (bmSynced) y += 6;  // separate from the bookmark block above
        snprintf(buf, sizeof(buf), tr(STR_SYNC_DICT_FORMAT), dictUploadedWords, dictUploadedDeletes, dictMergedWords,
                 dictDeletedWords);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
        char dStatus[64];
        snprintf(dStatus, sizeof(dStatus), tr(STR_BOOKMARK_SYNC_STATUS_FORMAT),
                 statsFetchOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER),
                 statsUploadOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER));
        snprintf(buf, sizeof(buf), "  %s", dStatus);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
        y += lhFoot + 2;
      } else if (dictSkippedLowHeap) {
        if (bmSynced) y += 6;
        renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_SYNC_DICT_SKIPPED));
        y += lhFoot + 2;
      }
      if (statsSynced) {
        if (bmSynced || dictSynced || dictSkippedLowHeap) y += 6;  // reading time sits at the bottom
        char durBuf[24];
        BookReadingStats::formatDuration(statsTotalAllDevices, durBuf, sizeof(durBuf));
        snprintf(buf, sizeof(buf), tr(STR_STATS_ALL_DEVICES_FORMAT), durBuf);
        renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    // Wrap the detail (statusMessage may hold the long errorString, e.g. the
    // LOW_MEMORY text) over up to 3 lines instead of a single centered line that
    // runs off both screen edges.
    const auto detailLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto detailLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), screen.width - 40, 3);
    int detailY = top + 40;
    for (const auto& line : detailLines) {
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, detailY, line.c_str());
      detailY += detailLineHeight;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    // After a successful upload, return to the reader on its own once the user has
    // had a moment to read the confirmation — no manual Back needed.
    if (state == UPLOAD_COMPLETE && millis() - uploadCompleteAt >= UPLOAD_COMPLETE_AUTO_RETURN_MS) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
