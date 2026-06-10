#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>

#include "BookmarkStore.h"
#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

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
  returnToReader();
}

void KOReaderSyncActivity::returnToReader() {
  // The auto-return check in loop() is level-triggered, so guard against re-entry: fire the
  // reader switch exactly once.
  if (returning) return;
  returning = true;
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

  // Sync bookmarks alongside progress whenever the server is reachable (OK or NOT_FOUND).
  // Silent and best-effort: it never changes the progress sync outcome below.
  if (result == KOReaderSyncClient::OK || result == KOReaderSyncClient::NOT_FOUND) {
    syncBookmarks();
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
    return static_cast<int>(std::count_if(bms.begin(), bms.end(), [](const struct Bookmark& b) { return !b.returnMark; }));
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
  char syncHeader[72];
  if (activeServer && !activeServer->name.empty()) {
    snprintf(syncHeader, sizeof(syncHeader), "%s - %s", tr(STR_KOREADER_SYNC), activeServer->name.c_str());
  } else {
    snprintf(syncHeader, sizeof(syncHeader), "%s", tr(STR_KOREADER_SYNC));
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
    // Show comparison
    top = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 40, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 65, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 90, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 115, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 150, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 175, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 200, localPageStr);

    // Bookmark sync summary (merge is automatic; this is informational). Show it whenever a
    // sync was attempted so the upload status is visible even with no bookmarks to merge.
    int optionY = top + 230;
    if (bmSynced) {
      // Extra gap separates the bookmark block from the local progress above it.
      char bmStr[96];
      snprintf(bmStr, sizeof(bmStr), tr(STR_BOOKMARK_DIFF_FORMAT), bmRemoteCount, bmLocalCount, bmMergedCount);
      char bmStatusStr[96];
      snprintf(bmStatusStr, sizeof(bmStatusStr), tr(STR_BOOKMARK_SYNC_STATUS_FORMAT),
               bmFetchOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER),
               bmUploadOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER));
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 240, tr(STR_BOOKMARKS), true);
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 262, bmStr);
      renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, top + 284, bmStatusStr);
      optionY = top + 312;
    }
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(screen.x, optionY - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY, tr(STR_APPLY_REMOTE),
                      selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(screen.x, optionY + optionHeight - 2, screen.width - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, optionY + optionHeight,
                      tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 40, tr(STR_UPLOAD_PROMPT));

    if (bmSynced) {
      char bmStr[96];
      snprintf(bmStr, sizeof(bmStr), tr(STR_BOOKMARK_DIFF_FORMAT), bmRemoteCount, bmLocalCount, bmMergedCount);
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 90, bmStr);
      char bmStatusStr[96];
      snprintf(bmStatusStr, sizeof(bmStatusStr), tr(STR_BOOKMARK_SYNC_STATUS_FORMAT),
               bmFetchOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER),
               bmUploadOk ? tr(STR_OK_BUTTON) : tr(STR_FAILED_LOWER));
      UITheme::drawCenteredText(renderer, screen, UI_10_FONT_ID, top + 115, bmStatusStr);
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
