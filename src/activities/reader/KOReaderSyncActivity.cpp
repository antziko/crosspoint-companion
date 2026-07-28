#include "KOReaderSyncActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <WiFi.h>
#include <HalClock.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>
#include <cmath>

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
#include "util/FlashcardDeck.h"
#include "util/LookupHistory.h"

namespace {
// Format a transfer byte count for the summary: < 1 MB shows 2-decimal KB,
// otherwise 2-decimal MB (1024 base). Writes into the caller's fixed buffer.
void formatXferBytes(uint32_t bytes, char* out, size_t outLen) {
  if (bytes < 1024u * 1024u) {
    snprintf(out, outLen, "%.2f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(out, outLen, "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
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

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::completeAlreadySynced() {
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  uploadCompleteAt = millis();  // reuse the UPLOAD_COMPLETE auto-return countdown
  requestUpdate(true);
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

  // The renderer's FontCacheManager is a global that outlives EpubReaderActivity (whose
  // onExit() clears neither), so an SD-card font's retained mini-data (#2611-E keep-if-fits)
  // plus the FontDecompressor page slots stay resident straight into this sync. That few-KB
  // creep is now enough to drop the post-WiFi heap under MIN_HEAP_FOR_TLS (55000) and reject
  // the bookmark/progress TLS handshake. This activity never renders book text, and onExit()
  // always reboots, so the cache is rebuilt fresh on the next reader open -- free it here for
  // the handshake, symmetric with the inflate-window release above.
  if (auto* fcm = renderer.getFontCacheManager()) {
    const uint32_t before = ESP.getFreeHeap();
    fcm->clearCache();
    LOG_DBG("KOSync", "Cleared font caches for TLS (heap: %u -> %u)", (unsigned)before, (unsigned)ESP.getFreeHeap());
  }

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Sync time with NTP before making API requests. Delegate to HalClock: it drives SNTP via the
  // framework's configTzTime() (correct TCPIP core-lock handling), unlike the old inline esp_sntp_*
  // sequence which deadlocked — esp_sntp_setservername() self-locks the (non-recursive) core mutex,
  // so wrapping it in a manual LOCK_TCPIP_CORE() blocked forever. We own the WiFi connection here,
  // so give SNTP the full 5s budget; a late packet is still adopted asynchronously by HalClock.
  halClock.syncFromNTP(5000);

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

void KOReaderSyncActivity::performSync() {
  // Zero the cumulative GET/PUT transfer counters so the summary reflects only this sync.
  KOReaderSyncClient::resetByteCounters();

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

  // Phased-status step counter: ALL runs three network legs (progress, bookmarks,
  // stats); single-feature scopes run one (prefix is then hidden — see setSyncPhase).
  syncStepTotal = (syncScope == SyncScope::All) ? 3 : 1;
  syncStepIndex = 0;

  // Connection strategy. A keep-alive session wraps the small-body legs (progress + bookmarks):
  // they reuse ONE connection = ONE handshake, instead of each paying a fresh cold handshake
  // that needs a clean ~33KB contiguous block (only reliably present on a pristine heap — when
  // the heap is settled the largest block sits at ~32756, ~644 B short, so a second fresh
  // handshake -0x7F00s). The asymmetric TLS build shrinks OUT to 2KB so the held arena is small
  // and reuse no longer starves the write. Stats runs OUTSIDE the session at recovered heap —
  // its dict-history/global bodies and its own handshake want maximum room. See syncBookmarks /
  // syncStats.

  // Single-feature scopes skip the progress comparison entirely: run just the one
  // feature, then show the FEATURE_DONE summary. Stats, Dictionary and Flashcards
  // share the stats endpoint but gate independently — Stats syncs counters (+ global),
  // Dictionary syncs only the per-book "dh" history, Flashcards only the per-book "fc"
  // deck; the single-feature scopes skip the extra global round-trips.
  if (syncScope != SyncScope::All && syncScope != SyncScope::Progress) {
    switch (syncScope) {
      case SyncScope::Bookmarks:
        syncBookmarks();  // sessionless: GET/merge/PUT each at recovered heap (see syncBookmarks)
        break;
      case SyncScope::Stats:
        // no session: full heap for global body
        syncStats(/*includeDict=*/false, /*includeGlobal=*/true, /*includeFlashcards=*/false);
        break;
      case SyncScope::Dict:
        // no session: full heap for the dh body
        syncStats(/*includeDict=*/true, /*includeGlobal=*/false, /*includeFlashcards=*/false);
        break;
      case SyncScope::Flashcards:
        // no session: full heap for the fc body
        syncStats(/*includeDict=*/false, /*includeGlobal=*/false, /*includeFlashcards=*/true);
        break;
      default:
        break;
    }
    {
      RenderLock lock(*this);
      state = FEATURE_DONE;  // stays until the user presses Back (no auto-return)
    }
    requestUpdate(true);
    return;
  }

  syncStepIndex = 1;  // progress leg
  setSyncPhase(tr(STR_FETCH_PROGRESS));

  // Progress runs in a short keep-alive session (just the GET here). The session CLOSES at the
  // end of this block so bookmarks AND stats below run at recovered heap. Bookmarks USED to run
  // inside this session to share the handshake, but their merge (parseFromJson + mergeFrom build
  // several throwing std::vector reserves) needs free heap the held ~55KB arena denies — that
  // abort()ed mid-merge in "sync all" (HW 2026-06-22: mergeFrom reserve 4752B, largest 4596).
  // So bookmarks are now sessionless like stats: their GET/merge/PUT each run at recovered heap,
  // where the merge has room and a fresh handshake has its contiguous block (proven by stats'
  // sessionless GET+PUT both succeeding here).
  KOReaderSyncClient::Error result;
  {
    KOReaderSyncClient::SyncSession session;
    result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  }  // session closed: arena freed, heap recovers for bookmarks + stats' fresh handshakes

  // Full sync (ALL) also merges bookmarks whenever the server is reachable (OK or NOT_FOUND).
  // Silent and best-effort: it does not change the progress sync outcome below. Sessionless, at
  // recovered heap — see syncBookmarks.
  if (syncScope == SyncScope::All && (result == KOReaderSyncClient::OK || result == KOReaderSyncClient::NOT_FOUND)) {
    syncStepIndex = 2;  // bookmarks leg
    syncBookmarks();
  }

  // PROBE: heap state right before stats' cold handshake (needs ~33.4KB contiguous = two
  // ~16.7KB record buffers). If `largest` here stays well below ~33KB across runs, the prior
  // legs fragmented the heap and smaller TLS record buffers are the only real fix; ~33KB+ means
  // recovery works and stats handshakes.
  {
    const unsigned freeAfter = (unsigned)ESP.getFreeHeap();
    const unsigned largestAfter = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    LOG_INF("KOSync", "Pre-stats heap: free=%u largest=%u (stats handshake needs ~33KB contig)", freeAfter,
            largestAfter);
    SdDebugLog::log("KOSYNC", "pre-stats heap free=%u largest=%u (stats needs ~33KB contig)", freeAfter, largestAfter);
  }

  // Reading stats (ALL only), sessionless at recovered heap. PROGRESS scope skips it.
  if (syncScope == SyncScope::All && (result == KOReaderSyncClient::OK || result == KOReaderSyncClient::NOT_FOUND)) {
    syncStepIndex = 3;  // stats leg
    syncStats(/*includeDict=*/true, /*includeGlobal=*/true, /*includeFlashcards=*/true);
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (smartSyncEnabled()) {
      // Smart sync: no remote record for this document — upload local progress without prompting.
      LOG_DBG("KOSync", "Smart sync: no remote progress; uploading local %.6f", localProgress.percentage);
      hasRemoteProgress = false;
      performUpload();
      return;
    }
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

  // Prefer the exact spine/page from a crosspoint-sync rich position (lossless
  // CrossPoint<->CrossPoint sync); fall back to the approximate XPath mapping for
  // plain kosync servers or when the rich position cannot be applied.
  std::optional<CrossPointPosition> richMapped;
  if (remoteProgress.position.has_value()) {
    richMapped = ProgressMapper::fromRichPosition(epub, *remoteProgress.position, renderer);
  }
  if (richMapped.has_value()) {
    remotePosition = *richMapped;
  } else {
    SavedProgressPosition koPos = {remoteProgress.progress, remoteProgress.percentage};
    remotePosition = ProgressMapper::toCrossPoint(epub, koPos, renderer, currentSpineIndex, totalPagesInSpine);
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  if (smartSyncEnabled()) {
    // Smart sync: auto-resolve on the furthest-progress-wins rule (CrossPoint has no local
    // progress timestamps to do true LWW). No alternate-hash probe here — uploads/reads stay on
    // the user's configured match method, keeping the progress leg to a single handshake.
    static constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points
    const float delta = localProgress.percentage - remoteProgress.percentage;
    LOG_DBG("KOSync", "Smart decision: local=%.6f remote=%.6f delta=%.6f mapped=%d/%d", localProgress.percentage,
            remoteProgress.percentage, delta, remotePosition.spineIndex, remotePosition.pageNumber);
    if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
      completeAlreadySynced();
      return;
    }
    if (delta > 0) {
      performUpload();  // local ahead: push it up
      return;
    }
    saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);  // remote ahead: apply it
    return;
  }

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

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;

  // Rich CrossPoint position for crosspoint-sync servers (lossless CrossPoint<->
  // CrossPoint sync); plain kosync servers ignore the extra field.
  {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    pos.pageNumber = static_cast<uint16_t>(currentPage);
    pos.totalPages = static_cast<uint16_t>(totalPagesInSpine > 0 ? totalPagesInSpine : 1);
    pos.paragraphIndex = currentParagraphIndex;
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls (and may already be null on
    // entry). Reload it here to read title/author, and guard against a failed reload.
    // Filename is derived from the path and is always safe. (#2608)
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    progress.metadata = std::move(meta);
  }

  // Release epub before the TLS handshake to free ~30KB RAM. Nothing below needs it.
  epub.reset();

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

void KOReaderSyncActivity::setSyncPhase(const char* phase) {
  char buf[96];
  if (syncStepTotal > 1) {
    snprintf(buf, sizeof(buf), tr(STR_SYNC_STEP_FORMAT), syncStepIndex, syncStepTotal, phase);
  } else {
    snprintf(buf, sizeof(buf), "%s", phase);
  }
  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = buf;
  }
  // Block until painted: the caller proceeds into a multi-second blocking network
  // leg next, and the message must be on screen before that stall begins.
  requestUpdateAndWait();
}

void KOReaderSyncActivity::syncBookmarks() {
  // Sessionless: the GET, the merge (parseFromJson + mergeFrom), and the PUT each run on their own
  // fresh connection at recovered heap (the caller closed the progress session before calling
  // this). The merge builds several throwing std::vector reserves, so it MUST NOT run under a held
  // keep-alive arena (~16KB free) — that abort()ed mid-merge.
  //
  // ORDER IS HEAP-CRITICAL. Both handshakes must clear MIN_HEAP_FOR_TLS (55000), and the post-WiFi
  // ceiling is only ~56KB — a razor-thin margin. The GET needs NEITHER the epub NOR the local
  // bookmark set (those only feed the upload body), so it now runs FIRST, at the post-WiFi
  // high-water. The old order loaded the epub metadata + local bookmarks BEFORE the GET, spending
  // ~3KB resident and dropping the pre-handshake heap under the gate — the GET was rejected at
  // ~52.6KB even though the fetch needs none of that RAM (HW 2026-07-26: free=52620 -> REJECT).
  // Symmetric on the PUT: the ~KBs of in-memory bookmark/tombstone vectors are freed
  // (BOOKMARKS.unload() — non-destructive, flushes then clears) once the small upload body is
  // serialized, so the PUT handshake also runs at recovered heap. The upload JsonDocument is built
  // and torn down BEFORE the PUT (only the result string persists), so mbedtls_ssl_write's scratch
  // isn't fragmented at write time — the fragmentation concern behind the old pre-GET serialize.
  setSyncPhase(tr(STR_SYNC_PH_BM_FETCH));

  // Heap probe for the thin-margin troubleshooting: emit free + largest-contiguous to both serial
  // and the SD trace at each handshake boundary so a future rejection shows exactly which leg fell
  // short and by how much (the gate checks total free; largest exposes fragmentation separately).
  const auto logHeap = [](const char* where) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    LOG_DBG("KOSync", "BM heap @%s: free=%u largest=%u", where, (unsigned)ESP.getFreeHeap(),
            (unsigned)info.largest_free_block);
    SdDebugLog::log("KOSYNC", "BM heap @%s free=%u largest=%u", where, (unsigned)ESP.getFreeHeap(),
                    (unsigned)info.largest_free_block);
  };

  // --- GET first, at the post-WiFi high-water heap (before any epub / local load) ---
  logHeap("pre-get");
  std::string remoteJson;
  const auto getResult = KOReaderSyncClient::getBookmarks(documentHash, remoteJson);
  // NOT_FOUND just means the server has nothing stored yet — the fetch itself succeeded.
  bmFetchOk = (getResult == KOReaderSyncClient::OK || getResult == KOReaderSyncClient::NOT_FOUND);
  if (!bmFetchOk) {
    // Fetch failed, but still upload the local set below so the server learns our bookmarks.
    LOG_ERR("KOSync", "Bookmark fetch failed: %s", KOReaderSyncClient::errorString(getResult));
  }

  // Parse the remote blob into (small) vectors now so remoteJson can be freed before the local
  // set is loaded — holding both alongside the merge needlessly fragments the heap. Elaborated
  // type: BaseTheme.h's UIIcon enum has a 'Bookmark' enumerator that otherwise hides the struct.
  std::vector<struct Bookmark> remoteBms;
  std::vector<Tombstone> remoteTombs;
  bool haveRemote = false;
  if (getResult == KOReaderSyncClient::OK && BookmarkStore::parseFromJson(remoteJson.c_str(), remoteBms, remoteTombs)) {
    bmRemoteCount = static_cast<int>(remoteBms.size());
    haveRemote = true;
  }
  std::string().swap(remoteJson);

  // --- Load the local set + merge ---
  setSyncPhase(tr(STR_SYNC_PH_BM_MERGE));

  // Epub title/author are display metadata only (the bookmark file is keyed by the path CRC), so
  // a failed load is fine — proceed with empty strings. Release it before the PUT to free RAM;
  // performSync reloads it after syncBookmarks returns (progress mapping).
  ensureEpubLoaded();
  std::string bookTitle;
  std::string bookAuthor;
  if (epub) {
    bookTitle = epub->getTitle();
    bookAuthor = epub->getAuthor();
    epub.reset();
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

  if (haveRemote) {
    const size_t added = BOOKMARKS.mergeFrom(remoteBms, remoteTombs);  // self-persists
    LOG_DBG("KOSync", "Merged %u remote bookmark(s)", (unsigned)added);
  }
  // Remote vectors are consumed — free them before the upload serialize.
  std::vector<struct Bookmark>().swap(remoteBms);
  std::vector<Tombstone>().swap(remoteTombs);
  bmMergedCount = countSyncable();
  bmSynced = true;

  // --- Serialize the final (merged) upload body, then free the in-memory set for the PUT ---
  // Budget-cap so out.reserve() can't OOM-abort. An empty body would wipe the server set, so skip
  // the PUT if serialize fails/overflows — serializing the post-merge state directly means there
  // is no stale-subset risk (no pre-merge body to fall back to).
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t bodyBudget = largestBlock > 1024 ? largestBlock - 1024 : 0;
  std::string localJson =
      BookmarkStore::serializeToJson(BOOKMARKS.getBookmarks(), BOOKMARKS.getTombstones(), bodyBudget);

  // Drop the in-memory bookmark/tombstone vectors now: the PUT needs only the serialized string
  // above. unload() flushes any dirty state to disk first, so this is non-destructive. This
  // recovers the margin the GET reorder alone can't, so the PUT also clears MIN_HEAP_FOR_TLS.
  BOOKMARKS.unload();

  if (localJson.empty()) {
    LOG_ERR("KOSync", "Bookmark upload skipped: serialize produced empty body");
    bmUploadOk = false;
    return;
  }

  logHeap("pre-put");
  setSyncPhase(tr(STR_SYNC_PH_BM_UPLOAD));
  const auto putResult = KOReaderSyncClient::updateBookmarks(documentHash, localJson);
  bmUploadOk = (putResult == KOReaderSyncClient::OK);
  if (!bmUploadOk) {
    // A failed upload means local deletes/additions never reached the server, so other
    // devices won't converge. Surface this on the result screen rather than hiding it.
    LOG_ERR("KOSync", "Bookmark upload failed: %s", KOReaderSyncClient::errorString(putResult));
  }
}

void KOReaderSyncActivity::syncStats(bool includeDict, bool includeGlobal, bool includeFlashcards) {
  // Sessionless, deliberately — and the caller (performSync) closes the progress+bookmarks
  // session BEFORE calling this so heap has recovered. The stats PUTs can carry LARGE bodies:
  // a base64 "dh" dictionary-history blob (per-book, up to ~5.5KB) and a base64 "h" dated-
  // history blob (global). Those builds use throwing allocations and are gated on full heap
  // (kDictSyncMinHeap 48KB / kGlobalStatsMinHeap 32KB). Inside a held-arena session only ~18KB
  // is free, which would force both to skip every time (and risk an abort if they didn't).
  // A fresh connection at recovered heap gives each PUT room for its body and re-arms the
  // contigOkForPut gate, so an oversized body skips cleanly instead of crashing.

  setSyncPhase(tr(STR_SYNC_PH_STATS_FETCH));

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
  // includeDict gates dict by scope; the heap check is the OOM backstop on top.
  const bool doDictSync = includeDict && ESP.getFreeHeap() > kDictSyncMinHeap;
  std::unique_ptr<uint8_t[]> dictUp;
  size_t dictUpLen = 0;
  // Delta-vs-keyframe upload bookkeeping: serializeForUpload picks the blob and
  // reports what it covered; the watermark is advanced (commitUpload) only after a
  // confirmed PUT, so a failed upload re-sends the same range next time.
  bool dictSerialized = false;
  bool dictWasKeyframe = false;
  LookupHistory::BlobStats dictUpStats;
  struct DictMergeCtx {
    const std::string* cachePath;
    int merged;   // remote adds applied
    int deleted;  // remote deletes applied
  } dictMergeCtx{&cachePath, 0, 0};
  StatsDatedFold dictFold;
  if (doDictSync) {
    // Right-size the upload blob so the 4KB serialize scratch does NOT straddle the stats GET
    // handshake. The handshake needs ~33.4KB contiguous (two ~16.7KB record buffers); a
    // persistent 4KB blob fragments the largest free block below that and fast-fails ssl_setup
    // (-0x7F00) even as the first TLS op — hardware-confirmed: individual stats GET at
    // largest=32756 (~630 B short). So serialize pre-merge into a 4KB scratch, copy the actual
    // bytes into a tight buffer, and free the scratch BEFORE the GET. The real blob is small (a
    // few hundred bytes), so what persists across the handshake is tiny and the block stays
    // intact. Pre-merge upload semantics are unchanged.
    auto dictScratch = makeUniqueNoThrow<uint8_t[]>(kDictBlobCap);
    if (dictScratch) {
      const size_t n =
          LookupHistory::serializeForUpload(cachePath, dictScratch.get(), kDictBlobCap, &dictUpStats, &dictWasKeyframe);
      dictSerialized = true;
      dictUploadedWords = dictUpStats.histCount;
      dictUploadedDeletes = dictUpStats.tombCount;
      if (n > 0) {
        dictUp = makeUniqueNoThrow<uint8_t[]>(n);
        if (dictUp) {
          std::copy_n(dictScratch.get(), n, dictUp.get());
          dictUpLen = n;
        }
      }
    }
    // dictScratch frees at this block's end (before getStats): its 4KB returns to the heap so
    // the handshake gets a clean contiguous block.
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

  // Per-book flashcard cross-device merge (Lamport-versioned, rolling cursor).
  // Same shape as the dict block: serialize OUR pre-merge slice for upload (a
  // bounded rolling slice, sized adaptively from free heap + the last-seen device
  // count), free the scratch BEFORE the GET handshake, and merge every OTHER
  // device's "fc" blob during the GET. Gated on its OWN scope flag (independent of
  // dict) + the same heap backstop.
  const bool doFcSync = includeFlashcards && ESP.getFreeHeap() > kDictSyncMinHeap;
  std::unique_ptr<uint8_t[]> fcUp;
  size_t fcUpLen = 0;
  bool fcSerialized = false;
  FlashcardDeck::BlobStats fcUpStats;
  struct FcMergeCtx {
    const std::string* cachePath;
    int merged;   // remote cards added
    int deleted;  // remote deletes applied
  } fcMergeCtx{&cachePath, 0, 0};
  StatsDatedFold fcFold;
  if (doFcSync) {
    // Adaptive slice cap: grows with heap / shrinks with device count, clamped
    // under the 64 KB GET aggregate (see FlashcardDeck::adaptiveSliceCap).
    const FlashcardDeck::SyncWatermark fcWm = FlashcardDeck::loadWatermark(cachePath);
    const size_t fcCap = FlashcardDeck::adaptiveSliceCap(ESP.getFreeHeap(), fcWm.lastDeviceCount);
    auto fcScratch = makeUniqueNoThrow<uint8_t[]>(fcCap);
    if (fcScratch) {
      const size_t n = FlashcardDeck::serializeForUpload(cachePath, fcScratch.get(), fcCap, &fcUpStats);
      fcSerialized = true;
      fcUploadedCards = fcUpStats.histCount;
      fcHealCards = fcUpStats.rollCount;
      fcUploadedDeletes = fcUpStats.tombCount;
      if (n > 0) {
        fcUp = makeUniqueNoThrow<uint8_t[]>(n);
        if (fcUp) {
          std::copy_n(fcScratch.get(), n, fcUp.get());
          fcUpLen = n;
        }
      }
    }
    // fcScratch frees here (before getStats) so the handshake gets a clean block.
    fcFold.ctx = &fcMergeCtx;
    fcFold.fn = [](void* ctx, const uint8_t* blob, size_t len) {
      auto* c = static_cast<FcMergeCtx*>(ctx);
      int del = 0;
      c->merged += FlashcardDeck::mergeBlob(*c->cachePath, blob, len, &del);
      c->deleted += del;
    };
  } else {
    LOG_DBG("KOSync", "Low heap (%u); skipping flashcard sync", (unsigned)ESP.getFreeHeap());
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
  const auto getResult = KOReaderSyncClient::getStats(documentHash, entries, count, nullptr,
                                                      doDictSync ? &dictFold : nullptr, doFcSync ? &fcFold : nullptr);
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
  setSyncPhase(tr(STR_SYNC_PH_STATS_UPLOAD));
  // Upload our scalar counters + our (pre-merge) "dh" dictionary-history and "fc"
  // flashcard blobs.
  const auto putResult = KOReaderSyncClient::updateStats(
      documentHash, mine, nullptr, 0, dictUp ? dictUp.get() : nullptr, dictUpLen, fcUp ? fcUp.get() : nullptr, fcUpLen);
  statsUploadOk = (putResult == KOReaderSyncClient::OK);
  if (!statsUploadOk) {
    LOG_ERR("KOSync", "Stats upload failed: %s", KOReaderSyncClient::errorString(putResult));
    // SD-only (USB-locked X3 has no serial): PUT failed -> commitUpload below is
    // skipped, so flashcard/dict watermarks don't advance and the same blob re-uploads
    // next sync (the persistent "new:+N" symptom). Records the actual failure reason.
    SdDebugLog::log("KOSYNC", "STATS_PUT FAILED (%s) -> watermarks NOT advanced; fc/dict will re-upload",
                    KOReaderSyncClient::errorString(putResult));
  } else {
    // PUT confirmed: advance the upload watermarks so the next sync ships only
    // newer changes (dict) / the next rolling slice (flashcards). Done only on
    // success, so a failed upload re-sends the same range/slice next time.
    if (dictSerialized) LookupHistory::commitUpload(cachePath, dictUpStats, dictWasKeyframe);
    if (fcSerialized) FlashcardDeck::commitUpload(cachePath, fcUpStats, static_cast<uint32_t>(count));
  }

  // Server build clue for the page header: tag echoed by a stats-enabled server,
  // "stats" if the PUT succeeded against a tag-less stats build, "no stats" if the
  // endpoint doesn't exist (404: stock/legacy server). Transport errors leave it
  // empty — server build unknown.
  if (statsUploadOk) {
    const char* tag = KOReaderSyncClient::statsServerTag();
    // Drop the redundant "stats-" prefix the server echoes (e.g. "stats-v1" -> "v1");
    // the header context already implies stats. Bare "stats" / empty -> "stats".
    // Display-only: the tag never feeds the doc-id or any request, so this is cosmetic.
    if (strncmp(tag, "stats-", 6) == 0 && tag[6] != '\0') tag += 6;
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
  // Only a heap-forced skip counts as "skipped (low memory)". When dict was excluded
  // by scope (Stats-only sync), neither flag is set so the footer omits it entirely.
  dictSkippedLowHeap = includeDict && !doDictSync;
  dictMergedWords = dictMergeCtx.merged;
  dictDeletedWords = dictMergeCtx.deleted;

  // Flashcard merge result (same footer treatment as dict). fcSynced when attempted;
  // fcSkippedLowHeap only when excluded by heap, not by scope, so a missing line is
  // never silent on a Flashcards-scope run.
  fcSynced = doFcSync;
  fcSkippedLowHeap = includeFlashcards && !doFcSync;
  fcMergedCards = fcMergeCtx.merged;
  fcDeletedCards = fcMergeCtx.deleted;
  // Backfill progress: deck size (denominator) + the rolling-cursor position the
  // upload commit just advanced to (numerator). Read post-commit so it reflects the
  // stored watermark; on a failed PUT the cursor didn't advance and this shows the
  // unchanged position. Cursor wraps for continuous healing, so deck==cursor only
  // marks one full broadcast pass, not "nothing left ever".
  if (doFcSync) {
    fcDeckCount = FlashcardDeck::count(cachePath);
    fcCursor = static_cast<int>(FlashcardDeck::loadWatermark(cachePath).cursorIndex);
  }

  SdDebugLog::log("KOSync",
                  "stats sync: doc=%s local=%lu others=%lu fetch=%d upload=%d dictUp=%d/%d dictMerged=%d dictDel=%d "
                  "fcOn=%d fcUp=%d/%d fcMerged=%d fcDel=%d fcBytes=%u",
                  documentHash.c_str(), static_cast<unsigned long>(stats.totalReadingSeconds),
                  static_cast<unsigned long>(stats.remoteOtherSeconds), statsFetchOk ? 1 : 0, statsUploadOk ? 1 : 0,
                  dictUploadedWords, dictUploadedDeletes, dictMergedWords, dictDeletedWords, doFcSync ? 1 : 0,
                  fcUploadedCards, fcUploadedDeletes, fcMergedCards, fcDeletedCards, (unsigned)fcUpLen);

  // --- Global (all-books) counter, same per-device scheme under a reserved
  // pseudo-document. The name can't collide with real documents: binary-mode
  // hashes are 32 hex chars and filename-mode hashes are MD5 hex too.
  // Skipped for the Dictionary-only scope (includeGlobal=false): those extra
  // round-trips aren't dictionary data and just add connection cost.
  if (includeGlobal && statsUploadOk) {  // skip the extra round-trips when the server has no stats support
    // Heap backstop for the global phase: the dated-history fold allocates accumulators
    // and the PUT grows a base64 "h" body (throwing allocations). The per-PUT contig gate
    // already guards the upload, but skip the whole phase if heap is degraded (e.g. a large
    // per-book dict merge left it low) rather than risk the fold's allocations. The global
    // counter is monotonic and re-syncs next time. (No session: each leg is a fresh conn.)
    constexpr uint32_t kGlobalStatsMinHeap = 32 * 1024;
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < kGlobalStatsMinHeap) {
      LOG_ERR("KOSync", "Global stats skipped: low heap %u < %u", (unsigned)freeHeap, (unsigned)kGlobalStatsMinHeap);
      return;  // global is the last phase of syncStats — nothing after it
    }
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

int KOReaderSyncActivity::drawAlsoSyncedFooter(int sideX, int y, int lhFoot, bool showAlsoLabel) {
  // One line per feature, with generous spacing for legibility. A feature with no real
  // change collapses to "<name>  up to date"; full counts show only when something moved.
  // Per-feature fetch/upload status is a compact "ok/fail" suffix on the same line (no
  // separate indented status row). The doc-id probe lives in the SD debug log, not here.
  const uint32_t xferDown = KOReaderSyncClient::bytesDown();
  const uint32_t xferUp = KOReaderSyncClient::bytesUp();
  const bool anyFeature = bmSynced || statsSynced || dictSynced || dictSkippedLowHeap || fcSynced || fcSkippedLowHeap;
  if (!anyFeature && xferDown == 0 && xferUp == 0) return y;

  const int ROW = lhFoot + 6;  // breathing room between rows
  char buf[160];
  auto st = [&](bool ok) { return ok ? tr(STR_SYNC_STAT_OK) : tr(STR_SYNC_STAT_FAIL); };

  y += 10;  // SECTION_GAP above the block
  // "Also synced:" framing only fits the full sync, where progress is the main event and
  // these ride along. On a single-feature sync the feature IS the event, so the caller
  // passes showAlsoLabel=false and the rows render with no "Also" header.
  if (showAlsoLabel) {
    renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_ALSO_SYNCED), true, EpdFontFamily::BOLD);
    y += ROW;
  }

  if (bmSynced) {
    char counts[96];
    snprintf(counts, sizeof(counts), tr(STR_BOOKMARK_DIFF_FORMAT), bmRemoteCount, bmLocalCount, bmMergedCount);
    snprintf(buf, sizeof(buf), "%s  %s  %s/%s", tr(STR_BOOKMARKS), counts, st(bmFetchOk), st(bmUploadOk));
    renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
    y += ROW;
  }

  if (dictSynced) {
    const bool idle = !dictUploadedWords && !dictUploadedDeletes && !dictMergedWords && !dictDeletedWords;
    if (idle) {
      snprintf(buf, sizeof(buf), "%s  %s", tr(STR_SYNC_SCOPE_DICT), tr(STR_SYNC_UPTODATE));
    } else {
      char counts[96];
      snprintf(counts, sizeof(counts), tr(STR_SYNC_DICT_FORMAT), dictUploadedWords, dictUploadedDeletes,
               dictMergedWords, dictDeletedWords);
      // STR_SYNC_DICT_FORMAT already carries the "Dictionary" label; append the status.
      snprintf(buf, sizeof(buf), "%s  %s/%s", counts, st(statsFetchOk), st(statsUploadOk));
    }
    renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
    y += ROW;
  } else if (dictSkippedLowHeap) {
    renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_SYNC_DICT_SKIPPED));
    y += ROW;
  }

  if (fcSynced) {
    const bool idle = !fcUploadedCards && !fcUploadedDeletes && !fcMergedCards && !fcDeletedCards;
    if (idle) {
      snprintf(buf, sizeof(buf), "%s  %s", tr(STR_SYNC_SCOPE_FLASHCARDS), tr(STR_SYNC_UPTODATE));
    } else {
      char counts[96];
      // new = real new/changed cards; in = merged from peers. heal (rolling re-broadcast)
      // is intentionally not shown as a count — its meaningful view is the backfill line.
      snprintf(counts, sizeof(counts), tr(STR_SYNC_FC_FORMAT), fcUploadedCards, fcUploadedDeletes, fcMergedCards,
               fcDeletedCards);
      snprintf(buf, sizeof(buf), "%s  %s/%s", counts, st(statsFetchOk), st(statsUploadOk));
    }
    renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
    y += ROW;
    // Backfill sub-line ONLY while a broadcast pass is incomplete (cursor < deck). Once the
    // deck has been fully re-offered the heal keeps cycling silently — no line needed.
    if (fcDeckCount > 0 && fcCursor < fcDeckCount) {
      const int remaining = fcDeckCount - fcCursor;
      const int perSlice = (fcHealCards > 0) ? fcHealCards : 1;  // cursor advances by the heal slice
      const int roundsLeft = (remaining + perSlice - 1) / perSlice;
      snprintf(buf, sizeof(buf), tr(STR_SYNC_FC_BACKFILL), fcCursor, fcDeckCount, roundsLeft);
      renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
      y += ROW;
    }
  } else if (fcSkippedLowHeap) {
    renderer.drawText(UI_10_FONT_ID, sideX, y, tr(STR_SYNC_FC_SKIPPED));
    y += ROW;
  }

  if (statsSynced) {
    char durBuf[24];
    BookReadingStats::formatDuration(statsTotalAllDevices, durBuf, sizeof(durBuf));
    snprintf(buf, sizeof(buf), tr(STR_STATS_ALL_DEVICES_FORMAT), durBuf);
    renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
    y += ROW;
  }

  // Transfer totals last (network summary). Shows even on a progress-only sync.
  if (xferDown > 0 || xferUp > 0) {
    char downBuf[24];
    char upBuf[24];
    formatXferBytes(xferDown, downBuf, sizeof(downBuf));
    formatXferBytes(xferUp, upBuf, sizeof(upBuf));
    snprintf(buf, sizeof(buf), tr(STR_SYNC_XFER_FORMAT), downBuf, upBuf);
    renderer.drawText(UI_10_FONT_ID, sideX, y, buf);
    y += ROW;
  }
  return y;
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
  // Match method (Filename/Binary) — device-side config that keys the doc-id both
  // devices must share. Surfaced first so a mismatched method is visible at a glance —
  // Binary keys on file content, so device-optimized copies never converge (the
  // flashcard/dh/stats cross-device bug). See KOReaderDocumentId::calculateFromFilename.
  {
    const size_t len = strlen(syncHeader);
    const bool filename = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME;
    snprintf(syncHeader + len, sizeof(syncHeader) - len, " (%s)", filename ? tr(STR_FILENAME) : tr(STR_BINARY));
  }
  // Server build clue last (it's server-supplied): the tag the stats-enabled server
  // echoed (e.g. "v1"), or "no stats" when the PUT 404'd (stock/legacy server without
  // the extension). Empty until known.
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
    y = drawAlsoSyncedFooter(sideX, y, lhFoot);

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
    y = drawAlsoSyncedFooter(sideX, y, lhFoot);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FEATURE_DONE) {
    // Single-feature sync summary: a "Sync complete" title plus the shared
    // "Also synced" footer (which holds whichever feature actually ran). No progress
    // comparison, no Epub needed. Auto-returns to the reader from loop().
    const int sideX = screen.x + metrics.contentSidePadding;
    const int lhFoot = renderer.getLineHeight(UI_10_FONT_ID);
    int y = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawText(UI_12_FONT_ID, sideX, y, tr(STR_SYNC_FEATURE_DONE), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
    // Single-feature sync: the feature is the main event, so skip the "Also synced:" header.
    drawAlsoSyncedFooter(sideX, y, lhFoot, /*showAlsoLabel=*/false);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top,
                              state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED), true,
                              EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
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
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE ||
      state == FEATURE_DONE) {
    // Full-sync progress upload and the smart "already synced" outcome auto-return to the reader
    // once the user has had a moment to read the confirmation — no manual Back needed. Single-feature
    // syncs (FEATURE_DONE) deliberately do NOT auto-return: the user stays on the result summary until
    // they press Back, so an individual Bookmarks/Stats/Dict/Flashcards sync doesn't snap away on its own.
    if ((state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) &&
        millis() - uploadCompleteAt >= UPLOAD_COMPLETE_AUTO_RETURN_MS) {
      returnToReader();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ((state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) &&
         mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
      returnToReader();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    auto chooseSelected = [this] {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
      } else if (selectedOption == 1) {
        performUpload();
      }
    };

    {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const int top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      constexpr int optionHeight = 30;
      int touchedOption = -1;
      const auto touch = mappedInput.rowTouch(touchedOption, top + 230 - 2, optionHeight, 2);
      if (touch == MappedInputManager::RowTouch::Down) {
        if (selectedOption != touchedOption) {
          selectedOption = touchedOption;
          requestUpdate();
        }
        return;
      }
      if (touch == MappedInputManager::RowTouch::Tap) {
        selectedOption = touchedOption;
        chooseSelected();
        return;
      }
    }

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
      chooseSelected();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty) && ty > renderer.getScreenHeight() / 3 &&
        ty < renderer.getScreenHeight() * 2 / 3) {
      if (documentHash.empty()) {
        if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
          documentHash = KOReaderDocumentId::calculateFromFilename(epubPath);
        } else {
          documentHash = KOReaderDocumentId::calculate(epubPath);
        }
      }
      performUpload();
      return;
    }

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
