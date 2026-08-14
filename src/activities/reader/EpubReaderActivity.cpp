#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <Serialization.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../settings/DictionarySelectActivity.h"
#include "BookStatsActivity.h"
#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "FlashcardListActivity.h"
#include "FlashcardReviewActivity.h"
#include "GlobalReadingStats.h"
#include "HighlightActionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "LookedUpWordsActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderOptionsActivity.h"
#include "ReaderSettingsIO.h"
#include "ReaderUtils.h"
#include "ReadingTimeHistory.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SleepSyncPromptActivity.h"
#include "SyncScopeSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};

// Per-book orientation cache file (.crosspoint/epub_<hash>/orientation.bin).
// Byte 0 = version, byte 1 = orientation value.
constexpr uint8_t ORIENTATION_FILE_VERSION = 1;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// Name of the finished-books subfolder. The full path is relative to the book's
// own folder, so a book in "/readeck/foo.epub" finishes into "/readeck/read/",
// while a book at the card root still goes to "/read/" (back-compat).
constexpr char READ_SUBFOLDER[] = "read";

// True if the book already sits directly inside a "read" folder, i.e. its
// immediate parent directory is named "read" ("/read/x", "/readeck/read/x").
// Non-allocating-ish; cheap enough for loop(). Prevents re-moving finished books.
bool isInReadFolder(const std::string& path) {
  const size_t lastSlash = path.rfind('/');
  if (lastSlash == std::string::npos || lastSlash == 0) return false;  // root-level file
  const size_t prevSlash = path.rfind('/', lastSlash - 1);
  const std::string dirName = path.substr(prevSlash + 1, lastSlash - prevSlash - 1);
  return dirName == READ_SUBFOLDER;
}

// Pick a non-colliding destination inside the "read" subfolder of the book's own
// directory for a finished book. Mirrors the suffixing scheme used elsewhere:
// "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;
  // Parent dir without trailing slash: "" for a root file, "/readeck" for a
  // server-foldered book. readDir then becomes "/read" or "/readeck/read".
  const std::string parentDir = (lastSlash != std::string::npos) ? srcPath.substr(0, lastSlash) : "";
  const std::string readDir = parentDir + "/" + READ_SUBFOLDER;

  Storage.mkdir(readDir.c_str());
  std::string dstPath = readDir + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = readDir + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and all its sidecar state (cache dir, bookmarks, recents
// entry, resume pointer) into /read/ via relocateBookSidecars().
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  relocateBookSidecars(srcPath, dstPath);
}

bool isSnippetWhitespace(const std::string_view word) {
  if (word.empty()) return true;
  return std::all_of(word.begin(), word.end(),
                     [](const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; });
}

void buildBookmarkSnippet(const Page& page, char* out, const size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  size_t len = 0;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& block = line.getBlock();
    if (!block) continue;
    const uint16_t wordCount = block->wordCount();
    for (uint16_t wi = 0; wi < wordCount; wi++) {
      const char* wordStr = block->wordText(wi);  // NUL-terminated
      const size_t wordLen = block->wordTextLen(wi);
      if (isSnippetWhitespace(std::string_view(wordStr, wordLen))) continue;
      const size_t separatorLen = len > 0 ? 1 : 0;
      if (len + separatorLen + wordLen >= outSize) return;
      if (separatorLen > 0) out[len++] = ' ';
      memcpy(out + len, wordStr, wordLen);
      len += wordLen;
      out[len] = '\0';
    }
  }
}

// Persists `sessionSecs` of reading time to both per-book and global stats on exit.
// `dated` is true when a clock source (X3 DS3231 RTC, or X4 NTP-synced system clock)
// supplied a calendar date for this session; dated sessions feed the weekly/monthly/
// yearly/heatmap history, undated ones (no clock available) fall back to
// `unattributedSeconds` — counted in totals but not dated.
void recordReadingSession(const std::string& cachePath, BookReadingStats& bookStats, uint32_t sessionSecs, bool dated,
                          uint16_t year, uint8_t month, uint8_t day, uint8_t dayOfWeek, uint8_t hour, uint8_t minute) {
  if (sessionSecs > 0) {
    bookStats.totalReadingSeconds += sessionSecs;
    if (dated) {
      // "Last read on ..." stamp for the Vega hero card -- only set on dated
      // (clock-available) sessions, encoded the same way as ReadingTimeHistory's
      // heatmapAnchorDay so the UI can reuse its day-index formatting helpers.
      // An undated session further down leaves this untouched rather than
      // clobbering a known-good stamp with "unknown".
      bookStats.lastReadDayIndex = readingHistoryDayIndex(year, month, day);
      bookStats.lastReadHour = hour;
      bookStats.lastReadMinute = minute;

      auto bookHistory = makeUniqueNoThrow<ReadingTimeHistory>();
      if (bookHistory) {
        const std::string historyPath = cachePath + "/book_time_history.bin";
        ReadingTimeHistory::load(historyPath, *bookHistory);
        bookHistory->recordDay(year, month, day, dayOfWeek, sessionSecs);
        ReadingTimeHistory::save(historyPath, *bookHistory);
      }
    } else {
      bookStats.unattributedSeconds += sessionSecs;
    }

    auto global = makeUniqueNoThrow<GlobalReadingStats>();
    if (global) {
      GlobalReadingStats::load(*global);
      global->totalReadingSeconds += sessionSecs;
      if (dated) {
        global->history.recordDay(year, month, day, dayOfWeek, sessionSecs);
      } else {
        global->unattributedSeconds += sessionSecs;
      }
      global->save();
    }
  }
  bookStats.save(cachePath);
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // If the book was moved/renamed outside the firmware, re-key its orphaned cache dir
  // (progress, stats, sections) before setupCacheDir() creates a fresh empty one.
  tryRecoverBookCache(epub->getPath());

  // Reset the per-session image render-failure memory (upstream #1003 placeholders).
  // Orientation is applied below from the book's saved value (APP_STATE.activeOrientation).
  ImageBlock::clearSessionRenderFailures();

  // Lazy image extraction (#2611): section builds only header-probe images for
  // their dimensions; the first render of an image page pulls the file out of the
  // EPUB through this hook. Function pointer + context (not std::function) — this
  // feeds render-loop code. Cleared in onExit before the epub shared_ptr drops.
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  epub->setupCacheDir();
  ensureCacheContentId(epub->getPath(), epub->getCachePath());
  // First open of a device-tagged book (e.g. "book (X3).epub"): seed its fresh
  // cache with reading stats + heatmap/timeline from the untagged sibling. When
  // an import happens, arm the open sync prompt below (regardless of threshold) so
  // the freshly seeded book reconciles progress with the server.
  const bool importedSiblingStats = importSiblingStatsIfNew(epub->getPath(), epub->getCachePath());

  // Load this book's saved orientation; fall back to the global default if none.
  APP_STATE.activeOrientation = SETTINGS.orientation;
  {
    HalFile of;
    if (Storage.openFileForRead("ERS", epub->getCachePath() + "/orientation.bin", of)) {
      uint8_t odata[2];
      if (of.read(odata, 2) == 2 && odata[0] == ORIENTATION_FILE_VERSION &&
          odata[1] < CrossPointSettings::ORIENTATION_COUNT) {
        APP_STATE.activeOrientation = odata[1];
      }
    }
  }

  // Configure screen orientation for this book.
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, APP_STATE.activeOrientation);

  // Load per-book reader settings; seed from current globals on first open.
  {
    CrossPointSettings::ReaderOverride bookOverride;
    if (!ReaderSettingsIO::load(epub->getCachePath(), bookOverride)) {
      // First open — snapshot the current global render defaults for this book.
      bookOverride.active = true;
      bookOverride.fontFamily = SETTINGS.fontFamily;
      bookOverride.fontPointSize = SETTINGS.fontPointSize;
      bookOverride.lineSpacing = SETTINGS.lineSpacing;
      bookOverride.paragraphAlignment = SETTINGS.paragraphAlignment;
      bookOverride.hyphenationEnabled = SETTINGS.hyphenationEnabled;
      bookOverride.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
      bookOverride.screenMargin = SETTINGS.screenMargin;
      static_assert(sizeof(bookOverride.sdFontFamilyName) == sizeof(SETTINGS.sdFontFamilyName),
                    "sdFontFamilyName size mismatch");
      strncpy(bookOverride.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(bookOverride.sdFontFamilyName) - 1);
      bookOverride.sdFontFamilyName[sizeof(bookOverride.sdFontFamilyName) - 1] = '\0';
      if (!ReaderSettingsIO::write(epub->getCachePath(), bookOverride)) {
        LOG_ERR("ERS", "Failed to seed per-book reader settings");
      }
    }
    SETTINGS.setReaderOverride(bookOverride);
  }

  // Reload any SD-card font at this book's (override) size before the first layout,
  // so SD/CJK fonts honor the per-book font size like built-in fonts do.
  sdFontSystem.ensureLoaded(renderer);

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");

  readingStats = BookReadingStats::load(epub->getCachePath());
  sessionStartMs = millis();
  sessionPauseStartMs = 0UL;
  currentPageVisibleMs = 0UL;
  sessionIdleExcessSecs = 0;
  sessionCommittedSecs = 0;
  statsCheckpointPending = false;

  // Arm the open/wake sync prompt if enough reading has accrued since the last sync. Evaluated
  // here (session just started, so the count is the prior unsynced reading), but shown only after
  // the first page render — see loop(). After a sync the marker resets, so resuming via goToReader
  // from KOReaderSyncActivity won't re-arm. Pointless without credentials.
  // A first-open sibling-stats import also arms it (one-time event), bypassing the threshold/opt-in
  // gate so the freshly seeded book reconciles progress with the server immediately.
  openSyncPromptArmed_ = KOREADER_STORE.hasCredentials() &&
                         (importedSiblingStats || (SETTINGS.syncPromptOnOpen && syncPromptThresholdReached()));

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // The lazy-image extractor holds a raw pointer to this activity's epub; drop it
  // before the activity (and the shared_ptr) goes away (#2611).
  ImageBlock::setExtractor(nullptr, nullptr);

  if (epub && sessionStartMs > 0) {
    // Account the page being viewed at exit: fold its final visible segment into the page's
    // accumulated visible time, then apply the idle cap to that total before committing.
    accumulateVisibleSegment();
    accountIdleExcess(currentPageVisibleMs);
    currentPageVisibleMs = 0UL;

    // Final flush of any reading time the periodic checkpoints haven't persisted yet.
    commitReadingTime(0);

    sessionStartMs = 0UL;
    pageShownAtMs = 0UL;
  }

  // Restore global render settings view for home screen / settings / TXT reader.
  SETTINGS.clearReaderOverride();
  // Reload any SD-card font back to the global size so the home/library UI (which
  // does not re-sync the SD font itself) matches the global font size again.
  sdFontSystem.ensureLoaded(renderer);
  // Drop any size the dictionary added on top (ensureFontSize). ensureLoaded above only
  // reloads when the family changed, so a dictionary-only size otherwise stays resident for
  // the rest of the session, holding per-style tables mid-heap. Reloaded on the next lookup.
  sdFontSystem.releaseExtraSizes(renderer);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // Restore the global-default invariant for non-reader UI.
  APP_STATE.activeOrientation = SETTINGS.orientation;

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  BOOKMARKS.unload();
  // Flush any progress the debounce hasn't written yet, so a normal exit/sleep never loses pages
  // (only a hard power-off between debounced writes can). No-op when already current.
  if (epub && section &&
      (currentSpineIndex != lastSavedSpine_ || static_cast<int>(section->currentPage) != lastSavedPage_)) {
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  }

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  // Runs after the flush above so it overrides the (footnote) current position.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::onPause() {
  // Reader suspended while a sub-activity (menu, word-select, chapter select, stats, ...) is
  // foreground. Anchor the instant so onResume can exclude the gap from reading time.
  if (sessionStartMs == 0) return;
  sessionPauseStartMs = millis();
}

void EpubReaderActivity::onResume() {
  // Returning from a sub-activity: shift the session anchor forward by the suspended gap so
  // every wall-clock delta (commitReadingTime, sync delta) excludes the in-sub-activity time.
  if (sessionStartMs == 0 || sessionPauseStartMs == 0) return;
  sessionStartMs += millis() - sessionPauseStartMs;
  sessionPauseStartMs = 0UL;
}

void EpubReaderActivity::commitReadingTime(uint32_t minDeltaSecs) {
  if (!epub || sessionStartMs == 0) return;

  const uint32_t sessionSecs = static_cast<uint32_t>((millis() - sessionStartMs) / 1000UL);
  // Subtract idle-page excess so leaving the device on a page doesn't inflate reading
  // time. With the cap Off, sessionIdleExcessSecs is 0 and this equals wall-clock.
  const uint32_t effectiveSecs = (sessionSecs > sessionIdleExcessSecs) ? (sessionSecs - sessionIdleExcessSecs) : 0;

  // Determine effective minimum session threshold: per-book override wins unless
  // it is set to MIN_SESSION_USE_GLOBAL, in which case fall back to global setting.
  // Nothing is committed (checkpoints included) until the session crosses it, so
  // sub-threshold sessions still write no stats at all.
  const auto& ov = SETTINGS.getReaderOverride();
  const uint8_t thresholdIdx =
      (ov.active && ov.minSessionMinutes != CrossPointSettings::ReaderOverride::MIN_SESSION_USE_GLOBAL)
          ? ov.minSessionMinutes
          : SETTINGS.minSessionMinutes;
  constexpr size_t kMinSessCount = sizeof(CrossPointSettings::MIN_SESSION_SECONDS) / sizeof(uint16_t);
  const uint32_t thresholdSecs =
      (thresholdIdx < kMinSessCount) ? CrossPointSettings::MIN_SESSION_SECONDS[thresholdIdx] : 0;
  if (effectiveSecs < thresholdSecs) return;

  if (effectiveSecs <= sessionCommittedSecs) return;
  const uint32_t deltaSecs = effectiveSecs - sessionCommittedSecs;
  if (deltaSecs < minDeltaSecs) return;

  // Use the local calendar day (RTC raw date + SETTINGS.clockUtcOffsetQ), not the
  // RTC's raw date -- a session that starts just after local midnight must be
  // attributed to "today", not the RTC's still-previous UTC-ish day, or the
  // weekly/monthly/yearly/heatmap history buckets it under the wrong date.
  // Evaluated per commit: on X4 a mid-session NTP sync upgrades later deltas
  // from undated to dated.
  uint8_t dayOfWeek = 0, day = 0, month = 0, hour = 0, minute = 0;
  uint16_t year = 0;
  const bool dated = halClock.isAvailable() &&
                     halClock.getLocalDateTime(SETTINGS.clockUtcOffsetQ, dayOfWeek, day, month, year, hour, minute);
  recordReadingSession(epub->getCachePath(), readingStats, deltaSecs, dated, year, month, day, dayOfWeek, hour, minute);
  sessionCommittedSecs += deltaSecs;
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  // Below the floors: just wait. The tick is deferrable — page-turn transients
  // free up between turns and the tick retries every loop pass. Track the
  // paused state so skipLoopDelay() stops pinning the CPU at full speed while
  // no build work is actually happening (the gate can stay closed for a long
  // stretch if the retained build context itself holds the heap down).
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::drawIndexingPopup() {
  // Draw (or redraw) the indexing popup and arm its progress bar. Records the returned rect so the
  // build loops can fill the bar, and resets the throttle so the first fill repaints. The popup's own
  // refresh is a plain FAST, so force the page that replaces it onto the HALF ghost-cleanup path --
  // otherwise the "INDEXING" text and its bar ghost under the rendered page.
  indexingPopupRect_ = GUI.drawPopup(renderer, tr(STR_INDEXING));
  lastIndexingPct_ = -1;
  pagesUntilFullRefresh = CrossPointSettings::REFRESH_COUNTDOWN_FORCE_FULL;
}

void EpubReaderActivity::updateIndexingProgress() {
  // Only while the popup is actually on screen this pass and the framebuffer is ours to draw into.
  if (indexingPopupRect_.width <= 0 || !renderer.hasFrameBuffer() || !section) return;
  const int pct = section->buildProgressPercent();
  // -1 => build not reporting yet. Throttle to at most one repaint per INDEXING_PROGRESS_STEP percent
  // so a long index doesn't pay a full-panel FAST refresh on every chunk.
  if (pct < 0 || pct < lastIndexingPct_ + INDEXING_PROGRESS_STEP) return;
  lastIndexingPct_ = pct;
  GUI.fillPopupProgress(renderer, indexingPopupRect_, pct);
}

void EpubReaderActivity::showBuildPopup() {
  // Mid-build indexing popup: only during render()'s blocking build-to-target phase
  // (buildPopupPending), at most once, and only when the framebuffer isn't on loan.
  // If it were called while the loan is active the draw would be lost, so pending
  // stays set and the deadline check retries on the next chunk after the loan ends.
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  drawIndexingPopup();
  buildPopupPending = false;
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // Periodic reading-time checkpoint, requested by the render task on full-refresh
  // pages. Handled here on the main task (same context as onExit's final flush) so
  // the session counters stay single-task. A crash then loses at most the time
  // since the last full-refresh page instead of the whole session.
  if (statsCheckpointPending) {
    statsCheckpointPending = false;
    commitReadingTime(STATS_CHECKPOINT_MIN_SECS);
  }

  // Open/wake sync prompt: fire once, after the first page has actually rendered (pageShownAtMs
  // is set by renderContents), so the page paints first and a failed book load never prompts.
  if (openSyncPromptArmed_ && section && pageShownAtMs > 0) {
    openSyncPromptArmed_ = false;
    showOpenSyncPrompt();
    return;  // prompt pushed; resume handling next loop iteration
  }

  // Idle glyph prewarm for the likely next page (currentPage + 1). The scan
  // pass draws nothing (FCM scan mode suppresses pixels), so the displayed
  // framebuffer is untouched; endScanAndPrewarm loads only glyphs not already
  // cached. Debounced past rapid page-flipping, one attempt per position, and
  // deferred while a render/build owns the CPU or the heap is at the render
  // floor. Cross-chapter prewarm is deliberately out of scope (next spine's
  // section isn't loaded).
  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() &&
      lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
      ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock;  // the page table must not change under the scan
    // Re-check under the lock: peek() and acquisition are not atomic, so the render
    // task may have reset/replaced the section or moved the page in between.
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);  // scan only, no pixels
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
        }
      }
    }
  }

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from
  // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
  // session, so reopening a partial deliberately does NOT start it (see the deferral in
  // render()); crossing this margin is the signal that the reader will actually need pages
  // past the watermark soon. Uses the last render's viewport so pagination matches the
  // partial being extended.
  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
    if (!section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                             SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, buildViewportWidth,
                             buildViewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                             SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
      // the blocking extension in render(). Don't retry every tick.
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the window check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
  if (section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
      buildTickHeapGate()) {
    RenderLock lock;
    // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
    // build between the outer isBuilding() check and acquiring the lock here, in which case
    // buildSomeMore() would fail and wrongly reset the section. The heap gate must be re-read
    // too: a render that won the lock race can expand retained glyph buffers, invalidating the
    // pre-lock heap reading. cppcheck can't see the cross-task mutation, so it flags this as
    // always true.
    // cppcheck-suppress knownConditionTrueFalse
    if (section->isBuilding() && buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        // The chapter re-paginated since the saved progress (settings changed): we now know the
        // real page count, so re-render at the remapped page. No-op for an unchanged resume.
        requestUpdate();
      }
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Paged back into the book: drop the end screen's suggestion menu (its app +
  // theme tokens, ~2KB) so long sessions read with the smaller footprint.
  if (!atEndOfBook && endOfBookOptionsReady.load(std::memory_order_acquire)) {
    RenderLock lock(*this);
    endOfBookOptionsReady.store(false, std::memory_order_release);
    endOfBookOptions.reset();
  }

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // Hold-Confirm dispatch — mutually exclusive based on user setting.
  // Dictionary uses 600 ms (Dictionary::LONG_PRESS_MS); Bookmark uses 400 ms (ReaderUtils::BOOKMARK_HOLD_MS).
  if (section && mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (SETTINGS.holdConfirmAction == CrossPointSettings::HOLD_CONFIRM_DICTIONARY &&
        mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
      if (Dictionary::exists(epub->getCachePath().c_str())) {
        ignoreNextConfirmRelease = true;
        openWordSelect(/*framebufferContainsPage=*/true);
        return;
      }
      if (!showNoDictionaryMessage) {
        showNoDictionaryMessage = true;
        ignoreNextConfirmRelease = true;
        noDictionaryMessageTime = millis();
        requestUpdate();
      }
    }
    if (SETTINGS.holdConfirmAction == CrossPointSettings::HOLD_CONFIRM_BOOKMARK &&
        mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
      ignoreNextConfirmRelease = true;
      addBookmark(false, /*lightRefresh=*/true);
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    if (!bookmarkMessageLightRefresh) {
      // Popup path: re-render to clear the center popup off the page content.
      requestUpdate();
    }
    bookmarkMessageLightRefresh = false;
  }

  if (showNoDictionaryMessage && (millis() - noDictionaryMessageTime) >= ReaderUtils::DICTIONARY_MESSAGE_DURATION_MS) {
    showNoDictionaryMessage = false;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (atEndOfBook && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive() &&
      !(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    std::string openPath;
    switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Enter reader menu activity on short-press Confirm, the board's menu edge-swipe, or a
  // middle-third tap (see ReaderUtils::isTouchMenuGesture). A long-press
  // that fired a bound function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      openReaderMenu();
    }
  }

  // Suppress Back bleed-through after dictionary chain exit. Capture the flag BEFORE
  // updating it so the release frame itself is gated — otherwise the flag clears and
  // wasReleased(Back) fires onGoHome() on the same tick the user lets go.
  const bool suppressBack = ignoreBackUntilRelease;
  if (ignoreBackUntilRelease && !mappedInput.isPressed(MappedInputManager::Button::Back)) {
    ignoreBackUntilRelease = false;
  }

  // Long press BACK (>= Dictionary::LONG_PRESS_MS, matching the confirm-hold dictionary gesture)
  // starts a highlight selection on the current page. The child word-select swallows the release
  // that ends this launching hold (see its onEnter), so letting go doesn't immediately cancel;
  // ignoreBackUntilRelease is re-armed on return.
  if (!suppressBack && section && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS && !highlightHoldFired) {
    highlightHoldFired = true;  // one-shot until Back is released, so we don't relaunch each tick
    openHighlightSelect();
    return;
  }
  if (highlightHoldFired && !mappedInput.isPressed(MappedInputManager::Button::Back)) {
    highlightHoldFired = false;
  }

  // Short press BACK: restore footnote position first, else navigate. When
  // backShortToFileBrowser is set, a short Back goes to the file browser instead of home.
  // (Long-press Back is owned by the highlight-select gesture above, so ReaderUtils::
  // handleBackNavigation's long/short split is not used for the Epub reader.)
  if (!suppressBack && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < Dictionary::LONG_PRESS_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    if (SETTINGS.backShortToFileBrowser) {
      activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    } else {
      onGoHome();
    }
    return;
  }

  // After the open sync prompt is dismissed with Skip, the release of the answering Left/Right
  // button bleeds into the resumed reader as a page turn. Swallow it until all nav buttons are up.
  if (suppressPageTurnUntilRelease_) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Left) &&
        !mappedInput.isPressed(MappedInputManager::Button::Right) &&
        !mappedInput.isPressed(MappedInputManager::Button::PageBack) &&
        !mappedInput.isPressed(MappedInputManager::Button::PageForward)) {
      suppressPageTurnUntilRelease_ = false;
    }
    return;
  }

  // Short power-button press = quick footnote access (#1658), when the user has
  // mapped the power button to FOOTNOTES. Down-press combo is excluded so the
  // power+down gesture (handled elsewhere) isn't swallowed. In a footnote, the
  // same press returns to the saved reading position.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else if (currentPageFootnotes.size() == 1) {
      navigateToHref(currentPageFootnotes[0].href, true);
    } else if (currentPageFootnotes.size() > 1) {
      startActivityForResultNoThrow<EpubReaderFootnotesActivity>(
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& footnoteResult = std::get<FootnoteResult>(result.data);
              navigateToHref(footnoteResult.href, true);
            }
            requestUpdate();
          },
          renderer, mappedInput, currentPageFootnotes);
    }
    return;
  }

  // Touch page-turn (#2481): left third = previous, right third = next; OR'd with
  // the button/tilt result. The middle third is the reader-menu tap.
  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  // Manual turns can't outrun the panel, so the guard further down refuses to
  // start a turn while a render is in flight or inside a short post-turn gap.
  // But the press itself shouldn't be lost: it's latched into pendingManualTurn
  // and executed here, on the first idle tick after the guard clears.
  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      // The section was dropped after the latch (re-layout, build failure,
      // bookmark jump): the queued turn no longer names a page.
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    pageTurn(forward);
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt, fromSide] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Long-press behavior is configured separately for side vs front buttons.
  const uint8_t lpBehavior = fromSide ? SETTINGS.sideLongPressButtonBehavior : SETTINGS.longPressButtonBehavior;

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && lpBehavior == SETTINGS.CHAPTER_SKIP) {
    // Long-press back mid-chapter jumps to the chapter start first; only at the
    // chapter start does it cross into the previous chapter.
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && lpBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (APP_STATE.activeOrientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (APP_STATE.activeOrientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (longPress && lpBehavior == SETTINGS.BOOKMARK_AND_SYNC) {
    // Hold right (forward) = sync progress; hold left (back) = toggle bookmark.
    onReaderMenuConfirm(nextTriggered ? EpubReaderMenuActivity::MenuAction::SYNC
                                      : EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE);
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  // Refuse to START a turn while a render is in flight, OR within a short window
  // of the last turn. render() runs on its own task (renderTaskLoop) concurrently
  // with input; a slow AA/image page display lags behind fast taps, and a second
  // turn firing before the first commits its differential baseline writes the
  // panel twice -> two overlapping page segments. RenderLock::peek() catches a
  // render that has already taken the lock (mirrors the automatic-turn guard),
  // but there is a brief window between requesting a turn and the render task
  // acquiring the lock where peek() is still false — a mashed second tap slips
  // through there, which is what still triggered after slow image pages. The
  // lastPageTurnTime gap bridges that startup latency; after it, peek() takes
  // over for the rest of the (variable-length) render. The press is latched, not
  // dropped: the consume block above runs it on the first idle tick, so one
  // eager tap during a slow render still turns the page. Latching (assign, not
  // increment) means mashing collapses to a single queued turn.
  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    // A percentage jump targets a position, not a paragraph. Drop any anchor still pending
    // from an earlier bookmark jump so it cannot override this one.
    pendingParagraphAnchor = UINT16_MAX;
    section.reset();
  }
}

void EpubReaderActivity::openReaderMenu() {
  // A turn latched during a render must not fire after the menu round-trip:
  // the user has moved on to a different interaction.
  pendingManualTurn = 0;
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

  // Folder name of the *configured* dictionary for display in the menu. Deliberately
  // readDictPath, not activeDictPath: the menu reports the saved selection, so a
  // session override from the definition screen must not be shown here as if it were
  // the setting.
  const std::string activeDictName = DictUtils::dictDisplayName(Dictionary::readDictPath(epub->getCachePath().c_str()));

  // Opening the reader menu returns to the same page, so freeze the marker dwell across the
  // round-trip (excludes the in-menu time) instead of restarting it on return. If the menu
  // changes layout (font/orientation), the subsequent re-layout re-renders the page anyway.
  pauseMarkerDwell();
  accumulateVisibleSegment();
  startActivityForResultNoThrow<EpubReaderMenuActivity>(
      [this](const ActivityResult& result) {
        // Always apply orientation change even if the menu was cancelled
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      },
      renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
      APP_STATE.activeOrientation, !currentPageFootnotes.empty(), Dictionary::exists(epub->getCachePath().c_str()),
      std::move(activeDictName));
}

void EpubReaderActivity::openWordSelect(bool framebufferContainsPage) {
  auto pageForLookup = section ? section->loadPage(section->currentPage) : nullptr;
  if (!pageForLookup) {
    requestUpdate();
    return;
  }
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.getReaderScreenMargin();
  orientedMarginLeft += SETTINGS.getReaderScreenMargin();

  // Bottom reserved-area height (matches renderContents() at line 695-703). The
  // word-select activity uses this to clear exactly the strip we drew the
  // status-bar / auto-turn label into, so its first frame matches the menu
  // path which wipes everything via clearScreen + page->render.
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const int reservedBottomHeight =
      (automaticPageTurnActive &&
       (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()))
          ? std::max(
                SETTINGS.getReaderScreenMargin(),
                static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin))
          : std::max(SETTINGS.getReaderScreenMargin(), statusBarHeight);
  std::string nextPageFirstWord;
  if (section && section->currentPage < section->pageCount - 1) {
    auto nextPage = section->loadPage(section->currentPage + 1);
    if (nextPage && !nextPage->elements.empty()) {
      const auto it = std::find_if(nextPage->elements.begin(), nextPage->elements.end(),
                                   [](const auto& el) { return el->getTag() == TAG_PageLine; });
      if (it != nextPage->elements.end()) {
        const auto* firstLine = static_cast<const PageLine*>(it->get());
        const auto& block = firstLine->getBlock();
        if (block && block->wordCount() > 0) {
          nextPageFirstWord.assign(block->wordText(0), block->wordTextLen(0));
        }
      }
    }
  }
  const std::string bookCachePath = epub->getCachePath();
  // TOC chapter title for the current page, stored on any flashcard enrolled
  // from this lookup (same lookup as addBookmark / openHighlightSelect).
  std::string chapterTitle;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) chapterTitle = epub->getTocItem(tocIndex).title;
  // Append in-chapter page position (X/Y) so an enrolled flashcard shows where in
  // the chapter the lookup happened. Stored inside the chapter field (deck format
  // unchanged); FlashcardReviewActivity's chapter footer renders it as-is.
  if (section && section->pageCount > 0) {
    char pos[24];
    snprintf(pos, sizeof(pos), " %d/%d", section->currentPage + 1, section->pageCount);
    chapterTitle += pos;  // enroll() caps the chapter field to CHAPTER_MAX (80)
  }
  // Choose the marker band from this page's dwell BEFORE the dwell is consumed/reset below.
  const WordSelectNavigator::InitialMarker initialMarker = computeWordSelectMarker();
  pauseMarkerDwell();  // freeze the dwell across this word-select round-trip (excludes in-dict time)
  accumulateVisibleSegment();
  // An in-flight chapter build pins its parser and CSS parser for as long as that chapter is
  // being read — finalizeBuild() only runs once the chapter is fully consumed, so on device
  // logs BUILD-START appears and BUILD-DONE never does. The cost is stark: mid-build the reader
  // sits at free=35,952 largest=15,860, against free=71,936 largest=55,284 on a section served
  // from cache. Word-select then opens at 23,556 instead of 58,880, and the definition view on
  // top of it at 7,136 — which is where every dictionary crash has happened.
  //
  // suspendBuild() is the purpose-built release: it persists the laid-out pages as a partial
  // and drops the parser, and the build resumes from that watermark on the next page turn past
  // the built region. It was only ever called from ~Section(), so nothing freed this while the
  // reader stayed open.
  //
  // Gated rather than unconditional: a cached section is already the healthy profile and must
  // not pay the resume cost. First estimates, same standing as kMinFreeForWordSelect — the
  // thresholds sit between the two measured states with margin on both sides; tune from the
  // logged values.
  constexpr size_t kMinFreeForDict = 48 * 1024;
  constexpr size_t kMinBlockForDict = 24 * 1024;
  if (section && section->isBuilding() && !RenderLock::peek() &&
      (ESP.getFreeHeap() < kMinFreeForDict || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kMinBlockForDict)) {
    RenderLock lock;  // suspendBuild() mutates pageCount/build_/file, which the render task reads
    // Re-check under the lock: peek() and acquisition are not atomic, so the render task may
    // have finished or replaced the build in between (same reason as the idle-prewarm gate).
    if (section && section->isBuilding()) {
      section->suspendBuild();
      SdDebugLog::log("EPUB", "build suspended for dict: free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
  }

  // Nothrow: a CJK page leaves the heap at ~13 KB largest by the time this is pushed. On
  // failure pageForLookup is untouched (a failed nothrow new never runs the constructor, so
  // the move never happens) and the reader simply stays on the page.
  auto wordSelect = makeUniqueNoThrow<DictionaryWordSelectActivity>(
      renderer, mappedInput, std::move(pageForLookup), orientedMarginLeft, orientedMarginTop, bookCachePath,
      nextPageFirstWord, framebufferContainsPage, reservedBottomHeight, DictionaryWordSelectActivity::Mode::Dictionary,
      initialMarker, chapterTitle);
  if (!wordSelect) {
    LOG_ERR("EPUB", "OOM: DictionaryWordSelectActivity");
    // pauseMarkerDwell() above is safe to leave armed: preserveMarkerDwell_ makes the next
    // render resume the dwell from markerDwellPausedElapsedMs, which is exactly right for a
    // round-trip that never happened. The page is still on screen, so nothing to repaint.
    return;
  }
  startActivityForResult(std::move(wordSelect), [this](const ActivityResult&) {
    ignoreBackUntilRelease = true;
    requestUpdate();
  });
}

void EpubReaderActivity::openHighlightSelect() {
  if (!section) {
    requestUpdate();
    return;
  }

  int pageCount, currentPage;
  {
    RenderLock lock(*this);
    pageCount = section->pageCount;
    currentPage = section->currentPage;
  }
  if (pageCount == 0) {
    requestUpdate();
    return;
  }

  const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
  const float progress = static_cast<float>(currentPage) / static_cast<float>(pageCount);

  // No existing highlight on this page → straight to selection (original behaviour).
  if (!BOOKMARKS.hasQuoteForPage(spine, progress, pageCount)) {
    launchHighlightWordSelect();
    return;
  }

  // Find the first quote anchored on this page and its full preview text. Page match
  // mirrors BookmarkStore::hasQuoteForPage (spine + progress within the page slice).
  const float pageSlice = 1.0f / static_cast<float>(pageCount);
  const auto& bms = BOOKMARKS.getBookmarks();
  uint16_t qStartWord = 0, qEndWord = 0;
  std::string quoteText;
  for (size_t i = 0; i < bms.size(); i++) {
    const auto& b = bms[i];
    if (!b.quote || b.spineIndex != spine || b.progress < progress || b.progress >= progress + pageSlice) continue;
    qStartWord = b.startWord;
    qEndWord = b.endWord;
    if (!BOOKMARKS.readPreviewAt(i, quoteText) || quoteText.empty()) {
      quoteText = b.snippet;  // fall back to the resident teaser if .qtext is unavailable
    }
    break;
  }

  startActivityForResultNoThrow<HighlightActionActivity>(
      [this, spine, qStartWord, qEndWord](const ActivityResult& result) {
        ignoreBackUntilRelease = true;
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const int action = std::get<MenuResult>(result.data).action;
        if (action == HighlightActionActivity::ACTION_DELETE) {
          BOOKMARKS.removeQuoteByRange(spine, qStartWord, qEndWord);
          requestUpdate();
        } else {
          // ACTION_ADD_NEW: existing highlight kept, start a fresh selection.
          launchHighlightWordSelect();
        }
      },
      renderer, mappedInput, std::move(quoteText));
}

void EpubReaderActivity::launchHighlightWordSelect() {
  auto pageForSelect = section ? section->loadPage(section->currentPage) : nullptr;
  if (!pageForSelect) {
    requestUpdate();
    return;
  }

  int pageCount, currentPage;
  {
    RenderLock lock(*this);
    pageCount = section->pageCount;
    currentPage = section->currentPage;
  }
  if (pageCount == 0) {
    requestUpdate();
    return;
  }

  // Anchor captured at launch = the page being highlighted (same as addBookmark()).
  const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
  const float progress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
  std::string chapterTitle;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) chapterTitle = epub->getTocItem(tocIndex).title;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.getReaderScreenMargin();
  orientedMarginLeft += SETTINGS.getReaderScreenMargin();

  // Choose the marker band from this page's dwell BEFORE the dwell is consumed/reset below.
  const WordSelectNavigator::InitialMarker initialMarker = computeWordSelectMarker();
  pauseMarkerDwell();  // freeze the dwell across this word-select round-trip (excludes in-dict time)
  accumulateVisibleSegment();

  // Launched from the menu: framebuffer holds the menu, not the page, so a full repaint
  // (framebufferContainsPage=false, reservedBottomHeight=0) and HighlightRange mode.
  // Nothrow — see openWordSelect(); same heap conditions, same untouched-on-failure argument.
  auto highlightSelect = makeUniqueNoThrow<DictionaryWordSelectActivity>(
      renderer, mappedInput, std::move(pageForSelect), orientedMarginLeft, orientedMarginTop, epub->getCachePath(), "",
      false, 0, DictionaryWordSelectActivity::Mode::HighlightRange, initialMarker);
  if (!highlightSelect) {
    LOG_ERR("EPUB", "OOM: DictionaryWordSelectActivity (highlight)");
    return;
  }
  startActivityForResult(std::move(highlightSelect), [this, spine, progress, pageCount, currentPage,
                                                      chapterTitle](const ActivityResult& result) {
    ignoreBackUntilRelease = true;
    if (!result.isCancelled) {
      if (const auto* hr = std::get_if<HighlightRangeResult>(&result.data)) {
        const auto addRes = BOOKMARKS.addQuote(spine, progress, static_cast<uint16_t>(std::max(0, hr->startWordIndex)),
                                               static_cast<uint16_t>(std::max(0, hr->endWordIndex)), pageCount,
                                               chapterTitle.empty() ? nullptr : chapterTitle.c_str(),
                                               hr->previewText.c_str(), currentPage);
        if (addRes == BookmarkStore::AddResult::LimitReached) {
          RenderLock lock(*this);
          // drawPopup refreshes internally (BaseTheme.cpp:803); a second displayBuffer here
          // was a second full-panel FAST refresh of the same pixels.
          GUI.drawPopup(renderer, tr(STR_MARK_LIMIT));
          delay(900);
        }
      }
    }
    requestUpdate();
  });
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      // Any explicit selection supersedes the session-start resume/reflow anchor, including a
      // selection that resolves to the page already showing. One lock covers the whole decision:
      // RenderLock wraps a non-recursive mutex, so the branches must not take their own.
      RenderLock lock(*this);
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      if (!section || section->pageCount == 0) break;
      const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      if (BOOKMARKS.hasPointBookmarkForPage(spine, progress, section->pageCount)) {
        // Remove: update only the status-bar strip so the image stays untouched.
        BOOKMARKS.removeBookmarkForPage(spine, progress, section->pageCount);
        lightStatusBarRefresh();
      } else {
        // Add: same light-refresh path — tab appears without re-rendering the page.
        addBookmark(false, /*lightRefresh=*/true);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      startActivityForResultNoThrow<EpubReaderChapterSelectionActivity>(
          [this](const ActivityResult& result) {
            // Cancelled (Back / back gesture) steps one level back to the
            // reader menu rather than dropping to the reading surface.
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              const auto chapterResult = std::get<ChapterResult>(result.data);

              auto doNavigate = [this, chapterResult]() {
                RenderLock lock(*this);
                clearDeferredReposition();
                currentSpineIndex = chapterResult.spineIndex;
                pendingAnchor = chapterResult.anchor;
                nextPageNumber = 0;
                section.reset();
              };

              if (section && section->pageCount > 0 && chapterResult.spineIndex != currentSpineIndex) {
                const float bmProgress =
                    static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
                if (!BOOKMARKS.hasPointBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), bmProgress,
                                                       section->pageCount)) {
                  startActivityForResultNoThrow<ConfirmationActivity>(
                      [this, doNavigate](const ActivityResult& confirmResult) {
                        if (!confirmResult.isCancelled) {
                          addBookmark(/*returnMark=*/true, /*lightRefresh=*/true);
                        }
                        doNavigate();
                      },
                      renderer, mappedInput, tr(STR_CONFIRM_ADD_RETURN_MARK), "");
                  return;
                }
              }
              doNavigate();
            }
          },
          renderer, mappedInput, epub, spineIdx);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResultNoThrow<EpubReaderFootnotesActivity>(
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& footnoteResult = std::get<FootnoteResult>(result.data);
            navigateToHref(footnoteResult.href, true);
            requestUpdate();
          },
          renderer, mappedInput, currentPageFootnotes);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResultNoThrow<EpubReaderPercentSelectionActivity>(
          [this, initialPercent](const ActivityResult& result) {
            // Cancelled steps back to the reader menu, not to the page.
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              const int targetPercent = clampPercent(std::get<PercentResult>(result.data).percent);

              auto doNavigate = [this, targetPercent]() { jumpToPercent(targetPercent); };

              if (section && section->pageCount > 0 && targetPercent != initialPercent) {
                const float bmProgress =
                    static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
                if (!BOOKMARKS.hasPointBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), bmProgress,
                                                       section->pageCount)) {
                  startActivityForResultNoThrow<ConfirmationActivity>(
                      [this, doNavigate](const ActivityResult& confirmResult) {
                        if (!confirmResult.isCancelled) {
                          addBookmark(/*returnMark=*/true, /*lightRefresh=*/true);
                        }
                        doNavigate();
                      },
                      renderer, mappedInput, tr(STR_CONFIRM_ADD_RETURN_MARK), "");
                  return;
                }
              }
              doNavigate();
            }
          },
          renderer, mappedInput, initialPercent);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          // QR display has no confirm path — its only exit is Back, so it
          // always steps back to the reader menu.
          startActivityForResultNoThrow<QrDisplayActivity>([this](const ActivityResult&) { openReaderMenu(); },
                                                           renderer, mappedInput, fullText);
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      startActivityForResultNoThrow<ConfirmationActivity>(
          [this](const ActivityResult& confirmResult) {
            if (confirmResult.isCancelled) {
              return;
            }
            {
              RenderLock lock(*this);
              if (epub && section) {
                uint16_t backupSpine = currentSpineIndex;
                uint16_t backupPage = section->currentPage;
                uint16_t backupPageCount = section->pageCount;
                section.reset();
                epub->clearCache();
                epub->setupCacheDir();
                if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
                  LOG_ERR("ERS", "Failed to save progress before cache clear");
                }
              }
            }
            onGoHome();
          },
          renderer, mappedInput, tr(STR_CONFIRM_DELETE_CACHE), "");
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        // Let the user pick what to sync (everything, or one feature). On a real
        // choice, launch the matching scope; on Back, do nothing.
        startActivityForResultNoThrow<SyncScopeSelectionActivity>(
            [this](const ActivityResult& result) {
              if (result.isCancelled) return;
              if (const auto* scopeResult = std::get_if<SyncScopeResult>(&result.data)) {
                launchKoSync(/*sleepWhenDone=*/false, scopeResult->scope);
              }
            },
            renderer, mappedInput);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP: {
      // Menu activity rendered over the page; the framebuffer no longer
      // matches what DictionaryWordSelectActivity expects.
      openWordSelect(/*framebufferContainsPage=*/false);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ADD_HIGHLIGHT: {
      openHighlightSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP_HISTORY: {
      auto history = makeUniqueNoThrow<LookedUpWordsActivity>(renderer, mappedInput, epub->getCachePath());
      if (!history) {
        LOG_ERR("EPUB", "OOM: LookedUpWordsActivity");
        openReaderMenu();
        break;
      }
      startActivityForResult(std::move(history), [this](const ActivityResult&) {
        ignoreBackUntilRelease = true;
        requestUpdate();
      });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::REVIEW_FLASHCARDS: {
      auto review = makeUniqueNoThrow<FlashcardReviewActivity>(renderer, mappedInput, epub->getCachePath());
      if (!review) {
        LOG_ERR("EPUB", "OOM: FlashcardReviewActivity");
        openReaderMenu();
        break;
      }
      startActivityForResult(std::move(review), [this](const ActivityResult&) {
        ignoreBackUntilRelease = true;
        requestUpdate();
      });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FLASHCARDS_LIST: {
      auto cards = makeUniqueNoThrow<FlashcardListActivity>(renderer, mappedInput, epub->getCachePath());
      if (!cards) {
        LOG_ERR("EPUB", "OOM: FlashcardListActivity");
        openReaderMenu();
        break;
      }
      startActivityForResult(std::move(cards), [this](const ActivityResult&) {
        ignoreBackUntilRelease = true;
        requestUpdate();
      });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SET_BOOK_DICTIONARY: {
      auto dictSelect = makeUniqueNoThrow<DictionarySelectActivity>(renderer, mappedInput, epub->getCachePath());
      if (!dictSelect) {
        LOG_ERR("EPUB", "OOM: DictionarySelectActivity");
        openReaderMenu();
        break;
      }
      startActivityForResult(std::move(dictSelect), [this](const ActivityResult&) { openReaderMenu(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS: {
      auto options = makeUniqueNoThrow<ReaderOptionsActivity>(
          renderer, mappedInput, epub->getCachePath(), SETTINGS.getReaderOverride(),
          /*showMinSession=*/true,
          // Seed the preview with the page the reader is on, so the user
          // previews font/size/margin changes against their own text.
          section ? section->getTextFromSectionFile() : std::string());
      if (!options) {
        LOG_ERR("EPUB", "OOM: ReaderOptionsActivity");
        openReaderMenu();
        break;
      }
      startActivityForResult(std::move(options), [this](const ActivityResult&) {
        // Re-layout: the per-book settings may have changed, so discard the
        // cached section and let render() rebuild it with the new parameters.
        RenderLock lock(*this);
        // Preserve current reading position so applyDeferredReposition() can
        // remap it onto the new pagination after reflow -- without this the
        // rebuild lands on a stale nextPageNumber and jumps the reader back.
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        // Reload any SD-card font at the new (override) size first; the
        // size-encoded font ID then forces the section cache to rebuild.
        sdFontSystem.ensureLoaded(renderer);
        section.reset();
        // The options screen exits on a Back press; swallow the matching
        // release so it doesn't bubble up to the reader's onGoHome().
        ignoreBackUntilRelease = true;
      });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS: {
      startActivityForResultNoThrow<EpubReaderBookmarksActivity>(
          [this](const ActivityResult& result) {
            // The bookmark list is a full-screen activity. On return the reader repaints via
            // the normal FAST_REFRESH cadence, which can't clear the full-screen list image —
            // it ghosts through ("frozen list"). Force the next text-page paint onto the
            // HALF_REFRESH ghost-cleanup path so the list is wiped cleanly. (Image/grayscale
            // pages take their own refresh path and are unaffected.)
            pagesUntilFullRefresh = CrossPointSettings::REFRESH_COUNTDOWN_FORCE_FULL;
            if (!result.isCancelled) {
              const auto& bm = std::get<BookmarkResult>(result.data);

              auto doNavigate = [this, bm]() {
                // Reopening the "return here" mark consumes it: the one-shot aid for getting
                // back has done its job, so drop it (no-op for a normal bookmark).
                BOOKMARKS.removeReturnMarkAt(bm.spineIndex, bm.paragraphIndex, bm.progress);
                RenderLock lock(*this);
                currentSpineIndex = bm.spineIndex;
                pendingSpineProgress = bm.progress;
                pendingPercentJump = true;
                // Prefer the recorded paragraph over the progress fraction so the jump
                // survives a re-layout; the fraction stays as the fallback.
                pendingParagraphAnchor = bm.paragraphIndex;
                section.reset();
              };

              if (section && section->pageCount > 0) {
                const float bmProgress =
                    static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
                if (!BOOKMARKS.hasPointBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), bmProgress,
                                                       section->pageCount)) {
                  startActivityForResultNoThrow<ConfirmationActivity>(
                      [this, doNavigate](const ActivityResult& confirmResult) {
                        if (!confirmResult.isCancelled) {
                          addBookmark(/*returnMark=*/true, /*lightRefresh=*/true);
                        }
                        doNavigate();
                      },
                      renderer, mappedInput, tr(STR_CONFIRM_ADD_RETURN_MARK), "");
                  return;
                }
              }
              doNavigate();
            }
          },
          renderer, mappedInput, epub->getPath());
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOK_STATS: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int progressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      BookStatsActivity::SessionContext session;
      // Mirror commitReadingTime's effectiveSecs: subtract idle-page excess so "This session"
      // matches what gets persisted. The current page's dwell is accumulated in
      // currentPageVisibleMs but not yet committed to sessionIdleExcessSecs (that happens when
      // the page is left), so apply the cap to it live here. openReaderMenu() already folded the
      // open segment into currentPageVisibleMs (pageShownAtMs is 0 here); the pageShownAtMs term
      // is defensive for any other caller.
      const uint32_t sessionSecs =
          sessionStartMs > 0 ? static_cast<uint32_t>((millis() - sessionStartMs) / 1000UL) : 0UL;
      const unsigned long liveDwellMs = currentPageVisibleMs + (pageShownAtMs > 0 ? millis() - pageShownAtMs : 0UL);
      const uint32_t reducible = sessionIdleExcessSecs + computeIdleExcessSecs(liveDwellMs);
      session.elapsedSecs = sessionSecs > reducible ? sessionSecs - reducible : 0UL;
      // Live reading pace (avg real reading seconds per forward page) from the in-progress stats,
      // shown on the Book Stats summary. Fresher than the on-disk copy the activity reloads.
      session.pacePerPageSecs = readingStats.avgSecondsPerForwardPage;
      startActivityForResultNoThrow<BookStatsActivity>(
          [this](const ActivityResult&) {
            ignoreBackUntilRelease = true;
            requestUpdate();
          },
          renderer, mappedInput, epub->getTitle(), epub->getCachePath(), progressPercent, session);
      break;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  // Nothrow, and home is the only honest fallback: the epub was released just above, so there
  // is no reader left to return to if this allocation fails.
  auto sync = makeUniqueNoThrow<KOReaderSyncActivity>(renderer, mappedInput, savedEpubPath, currentSpineIndex,
                                                      currentPage, totalPages, std::move(localKoPos),
                                                      std::move(localChapterName), paragraphIndex);
  if (!sync) {
    LOG_ERR("KOSync", "OOM: KOReaderSyncActivity; returning home");
    activityManager.goHome();
    return true;
  }
  activityManager.replaceActivity(std::move(sync));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches the book's current orientation.
  if (APP_STATE.activeOrientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist per-book so this book keeps the new orientation on next launch.
    // The global default (SETTINGS.orientation) is intentionally left untouched.
    APP_STATE.activeOrientation = orientation;
    saveOrientation();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, APP_STATE.activeOrientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::saveOrientation() const {
  HalFile f;
  if (Storage.openFileForWrite("ERS", epub->getCachePath() + "/orientation.bin", f)) {
    const uint8_t data[2] = {ORIENTATION_FILE_VERSION, APP_STATE.activeOrientation};
    f.write(data, 2);
  } else {
    LOG_ERR("ERS", "Failed to save per-book orientation");
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

WordSelectNavigator::InitialMarker EpubReaderActivity::computeWordSelectMarker() const {
  using Marker = WordSelectNavigator::InitialMarker;
  // Auto page-turn drives dwell on a timer, not reading; the setting being off keeps the
  // legacy centred marker. A fresh/unknown page (pageShownAtMs == 0) has no meaningful dwell.
  if (!SETTINGS.dictMarkerDwellEnabled || automaticPageTurnActive) return Marker::Middle;
  if (markerDwellStartMs == 0) return Marker::Top;
  const uint32_t dwellSecs = static_cast<uint32_t>((millis() - markerDwellStartMs) / 1000UL);
  // Idle guard: when the idle-page cap is enabled, a dwell beyond the selected cap is treated
  // as idle/AFK rather than reading, so the marker falls back to the neutral middle band instead
  // of drifting to the bottom. Cap = Off disables the guard (raw dwell, legacy bands).
  const uint8_t capIdx = SETTINGS.pageIdleCapSeconds;
  constexpr size_t kCapCount = sizeof(CrossPointSettings::PAGE_IDLE_CAP_SECONDS) / sizeof(uint16_t);
  if (capIdx != 0 && capIdx < kCapCount && dwellSecs > CrossPointSettings::PAGE_IDLE_CAP_SECONDS[capIdx]) {
    return Marker::Middle;
  }
  constexpr size_t kT1Count = sizeof(CrossPointSettings::DICT_MARKER_T1_SECONDS) / sizeof(uint16_t);
  constexpr size_t kT2Count = sizeof(CrossPointSettings::DICT_MARKER_T2_SECONDS) / sizeof(uint16_t);
  const uint8_t t1Idx = SETTINGS.dictMarkerT1Idx < kT1Count ? SETTINGS.dictMarkerT1Idx : 0;
  const uint8_t t2Idx = SETTINGS.dictMarkerT2Idx < kT2Count ? SETTINGS.dictMarkerT2Idx : 0;
  const uint32_t t1 = CrossPointSettings::DICT_MARKER_T1_SECONDS[t1Idx];
  // Guard against a user setting T2 < T1, which would otherwise erase the middle band.
  const uint32_t t2 = std::max<uint32_t>(t1, CrossPointSettings::DICT_MARKER_T2_SECONDS[t2Idx]);
  if (dwellSecs <= t1) return Marker::Top;
  if (dwellSecs <= t2) return Marker::Middle;
  return Marker::Bottom;
}

void EpubReaderActivity::pauseMarkerDwell() {
  // Stash reading ms accrued on this page so the return render can resume from it, excluding the
  // word-select time. A fresh/unknown page (anchor 0) stashes 0 so it resumes from zero.
  markerDwellPausedElapsedMs = (markerDwellStartMs != 0) ? (millis() - markerDwellStartMs) : 0UL;
  preserveMarkerDwell_ = true;
}

uint32_t EpubReaderActivity::computeIdleExcessSecs(unsigned long dwellMs) const {
  const uint8_t idx = SETTINGS.pageIdleCapSeconds;
  if (idx == 0) return 0;  // Off — full wall-clock, no idle cap.
  constexpr size_t kCount = sizeof(CrossPointSettings::PAGE_IDLE_CAP_SECONDS) / sizeof(uint16_t);
  if (idx >= kCount) return 0;  // out of range (shouldn't happen) — treat as Off.
  const uint32_t capSecs = CrossPointSettings::PAGE_IDLE_CAP_SECONDS[idx];
  const uint32_t dwellSecs = static_cast<uint32_t>(dwellMs / 1000UL);
  // Only pages held past the idle threshold are capped; cap <= threshold so this can't
  // underflow.
  if (dwellSecs > CrossPointSettings::PAGE_IDLE_THRESHOLD_SECONDS) {
    return dwellSecs - capSecs;
  }
  return 0;
}

void EpubReaderActivity::accountIdleExcess(unsigned long dwellMs) {
  sessionIdleExcessSecs += computeIdleExcessSecs(dwellMs);
}

void EpubReaderActivity::accumulateVisibleSegment() {
  if (pageShownAtMs > 0) {
    currentPageVisibleMs += millis() - pageShownAtMs;
    pageShownAtMs = 0UL;
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  // A page turn is authoritative: it must not be undone by a resume/reflow anchor captured at
  // session start once the incremental build completes. Safe to lock here — every caller bails
  // out on RenderLock::peek() before reaching this point, so the lock is never already held.
  {
    RenderLock lock(*this);
    clearDeferredReposition();
  }
  if (isForwardTurn) {
    // Fold the final visible segment into the page's accumulated reading time (sub-activity
    // time is already excluded). The idle cap is applied to this total by the new-page render
    // branch (renderCurrentPage). The pace sample also uses the total, not just the last
    // segment, so a page interrupted by the menu/dictionary still reflects all the real reading
    // done on it (excluding the sub-activity time), and the outlier rejection still applies.
    if (pageShownAtMs > 0) {
      currentPageVisibleMs += millis() - pageShownAtMs;
      pageShownAtMs = 0UL;
    }
    constexpr unsigned long MIN_DWELL_MS = 2000UL;
    if (currentPageVisibleMs >= MIN_DWELL_MS) {
      const uint32_t dwellSecs = static_cast<uint32_t>(currentPageVisibleMs / 1000UL);
      // Outlier rejection, but two guards against the old feedback trap where a single fast
      // page-flip seeded a tiny average that then rejected every real reading sample:
      //   (1) Warm-up: accept the first PACE_WARMUP_SAMPLES unconditionally so the average forms
      //       from real reading before any gating.
      //   (2) Loosened, floored gate: accept up to max(4*avg, idle threshold) instead of 2*avg.
      //       Genuinely-idle long pages are already handled by the idle-page cap, so the pace
      //       gate can be generous without re-admitting AFK pages.
      constexpr uint16_t PACE_WARMUP_SAMPLES = 5;
      const uint32_t avg = readingStats.avgSecondsPerForwardPage;
      const uint32_t acceptLimit = std::max<uint32_t>(4U * avg, CrossPointSettings::PAGE_IDLE_THRESHOLD_SECONDS);
      if (avg == 0 || readingStats.paceSampleCount < PACE_WARMUP_SAMPLES || dwellSecs <= acceptLimit) {
        readingStats.recordForwardPageRead(dwellSecs);
      }
    }

    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // No indexing popup carried over from a previous pass; the build sites below re-arm it as needed.
  indexingPopupRect_ = Rect{};
  lastIndexingPct_ = -1;

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this](const Section::BuildFailure& bf = Section::BuildFailure{}) {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    // Phase-A [E2] diagnostics: which build branch failed + the heap captured AT the failure point
    // (before the build's cleanup recovered it), so the number is real, not the post-reset heap.
    // Line 1: reason (+ ZipFile sub-reason for STREAM) + heap. Line 2: floor + inflated HTML size.
    // Only when a reason was recorded (createSectionFile/startBuild/buildSomeMore populated it).
    if (bf.reason != Section::BuildFailure::Reason::None) {
      char dbg[72];
      if (bf.reason == Section::BuildFailure::Reason::Stream) {
        snprintf(dbg, sizeof(dbg), "[E2/STREAM:%s spine=%d heap=%u]",
                 ZipFile::streamResultTag(static_cast<ZipFile::StreamResult>(bf.streamSub)), currentSpineIndex,
                 (unsigned)bf.failHeap);
      } else {
        snprintf(dbg, sizeof(dbg), "[E2/%s spine=%d heap=%u]", Section::buildFailureTag(bf.reason), currentSpineIndex,
                 (unsigned)bf.failHeap);
      }
      renderer.drawCenteredText(UI_12_FONT_ID, 330, dbg, true);
      char dbg2[64];
      snprintf(dbg2, sizeof(dbg2), "floor=%u html=%u", (unsigned)bf.floor, (unsigned)bf.htmlSize);
      renderer.drawCenteredText(UI_12_FONT_ID, 355, dbg2, true);
      renderer.displayBuffer();  // flush the appended diagnostics
    }
    automaticPageTurnActive = false;
  };

  // A section build failure: latch this spine so loop() doesn't retry the build every frame
  // (buildFailedSpine), capture the failure diagnostics before section.reset() recovers heap,
  // then surface the [E2] error overlay. Call with `section` still non-null.
  const auto failBuild = [this, &showBuildError, &showPendingSyncSaveError]() {
    buildFailedSpine = currentSpineIndex;  // stop the per-frame rebuild loop
    const Section::BuildFailure bf = section ? section->lastBuildFailure() : Section::BuildFailure{};
    section.reset();
    showBuildError(bf);
    showPendingSyncSaveError();
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    // Sole creation + load site: runs on the render task (serialized by
    // RenderLock); the main task only reads the suggestions once the loaded
    // flag is published. Created here so the app + theme tokens only exist
    // while the end screen shows; on OOM the end screen renders empty.
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) LOG_ERR("ERS", "OOM: EndOfBookOptions");
      // Release-publish AFTER construction so the main task's acquire load
      // can't observe a half-built object.
      endOfBookOptionsReady.store(endOfBookOptions != nullptr, std::memory_order_release);
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(epub->getPath());
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.getReaderScreenMargin();
  orientedMarginLeft += SETTINGS.getReaderScreenMargin();
  orientedMarginRight += SETTINGS.getReaderScreenMargin();

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.getReaderScreenMargin(),
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.getReaderScreenMargin(), statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  if (!section && currentSpineIndex == buildFailedSpine) {
    // This chapter already failed to index once (e.g. SD write error / card
    // full). Don't re-enter createSectionFile every frame — that spun forever on
    // "Indexing". Show an error; navigating to another chapter clears the flag.
    LOG_ERR("ERS", "Skipping rebuild of chapter %d that already failed to index", currentSpineIndex);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    {
      char dbg[48];
      snprintf(dbg, sizeof(dbg), "[E1 spine=%d heap=%u]", currentSpineIndex, (unsigned)esp_get_free_heap_size());
      renderer.drawCenteredText(UI_12_FONT_ID, 330, dbg, true);
    }
    // No renderStatusBar(): section is null here, and it dereferences section->.
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }
  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.getReaderExtraParagraphSpacing(),
        SETTINGS.getReaderParagraphAlignment(), viewportWidth, viewportHeight, SETTINGS.getReaderHyphenationEnabled(),
        SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled);
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      clearDeferredReposition();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Jumps that need the final pagination or the anchor map -- explicit page jumps,
      // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
      // resolve their landing page until the whole chapter is laid out, so they take the full
      // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
      // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
      // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
      // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
      // count is known, so it never blocks the first page.
      // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
      // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
      // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
      // without indexing the whole chapter.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        drawIndexingPopup();
        // No progress bar on this path: createSectionFile() runs the whole build under the
        // framebuffer loan (below), so mid-build repaints would be lost. Only the incremental
        // build-to-target path (the common first-open/deep-jump case) shows the animated bar.
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.getReaderExtraParagraphSpacing(),
                                        SETTINGS.getReaderParagraphAlignment(), viewportWidth, viewportHeight,
                                        SETTINGS.getReaderHyphenationEnabled(), SETTINGS.embeddedStyle,
                                        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          loan.end();  // restore before failBuild() draws the error overlay
          failBuild();
          return;
        }
        loan.end();
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Landing well inside a partial: the page (or anchor, via the on-disk map) is already
        // servable, so don't restart the extension build now -- it re-lays out the WHOLE chapter
        // from page 0 (minutes of background CPU + SD writes on a giant spine), pure waste when
        // the reader never nears the watermark this session. loop() starts it lazily once the
        // reader is within PARTIAL_REBUILD_START_MARGIN pages of the watermark.
        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            drawIndexingPopup();
          }
          // Mid-build popup surfacing for slow builds the up-front prediction can't
          // see (image extraction/probing per page, or the chunk loop overrunning
          // the deadline). buildPopupPending gates it to this blocking phase so a
          // background build in loop() can never draw over a displayed page.
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            // Lend the framebuffer's 48 KB to startBuild only (the spine HTML
            // inflation peak). The chunk loop below runs without it so the popup can
            // draw mid-build; the background buildSomeMore chunks in loop() never had
            // the loan either, so per-chunk layout is already known to fit.
            GfxRenderer::FrameBufferLoan loan(renderer);
            started =
                section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                    SETTINGS.getReaderExtraParagraphSpacing(), SETTINGS.getReaderParagraphAlignment(),
                                    viewportWidth, viewportHeight, SETTINGS.getReaderHyphenationEnabled(),
                                    SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled);
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            buildPopupPending = false;
            failBuild();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump ? !section->findAnchor(pendingAnchor) : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              // The up-front prediction guessed fast but the build blew the silent budget.
              showBuildPopup();
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              buildPopupPending = false;
              failBuild();
              return;
            }
            updateIndexingProgress();
          }
          buildPopupPending = false;
        }
      }
      buildFailedSpine = -1;      // built OK; allow this chapter again
      sectionJustRebuilt = true;  // built fresh this pass; page load must work now
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
      sectionJustRebuilt = false;  // using existing cache; one rebuild is allowed if it's stale
      buildFailedSpine = -1;       // loaded OK; allow this chapter again
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // Land on the bookmarked paragraph when one was recorded: it names the actual text, so
    // it stays correct across a re-layout that moved every page number. Falls through to
    // the percentage below when the section carries no page for it.
    if (pendingParagraphAnchor != UINT16_MAX && section->pageCount > 0) {
      const auto anchoredPage = section->getPageForParagraphIndex(pendingParagraphAnchor);
      if (anchoredPage.has_value() && *anchoredPage < section->pageCount) {
        section->currentPage = *anchoredPage;
        pendingPercentJump = false;
        LOG_DBG("ERS", "Bookmark paragraph %u -> page %u", pendingParagraphAnchor, *anchoredPage);
      } else {
        LOG_DBG("ERS", "Bookmark paragraph %u unresolved; using progress", pendingParagraphAnchor);
      }
      pendingParagraphAnchor = UINT16_MAX;
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  //
  // Crossing a partial's watermark before the extension rebuild has caught up means a
  // synchronous wait spanning the remaining prefix re-layout -- potentially tens of
  // seconds on a giant spine. Show the indexing popup so it isn't a silent freeze
  // (the page that replaces it takes the HALF ghost-cleanup path). Ordinary window
  // catch-ups on a non-partial build are a page or two and stay popup-free.
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    drawIndexingPopup();
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding() &&
        !section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                             SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                             SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                             SETTINGS.focusReadingEnabled)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
      updateIndexingProgress();
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
      updateIndexingProgress();
    }
  }

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    // Stale/overflowed reading position: re-pagination (different font, margins,
    // orientation, or a partial low-memory build) produced fewer pages than when
    // this position was saved, so currentPage now points past the last page.
    // Recover by clamping into range and rendering the nearest valid page instead
    // of dead-ending on an "Out of bounds" screen. pageCount >= 1 here (the
    // pageCount == 0 case returned above), so pageCount - 1 is a valid index.
    const int clamped = section->currentPage < 0 ? 0 : section->pageCount - 1;
    LOG_DBG("ERS", "Page out of bounds: %d (max %d) -> clamped to %d", section->currentPage, section->pageCount,
            clamped);
    section->currentPage = clamped;
  }

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        {
          // [E4] diagnostic: the page still won't load after abandoning the build, clearing the
          // cache, and exhausting retries. Reports spine + free heap for the USB-locked X3.
          char dbg[48];
          snprintf(dbg, sizeof(dbg), "[E4 spine=%d heap=%u]", currentSpineIndex, (unsigned)ESP.getFreeHeap());
          renderer.drawCenteredText(UI_12_FONT_ID, 330, dbg, true);
        }
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
    // EPUB steady-state heap profile (post-render, font cache already freed). Watch
    // `largest` for fragmentation and `minEver` for the worst-case low-water mark.
    LOG_DBG("MEM", "epub-page free=%u largest=%u minEver=%u", (unsigned)ESP.getFreeHeap(),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
    SdDebugLog::setEnabled(true);
    SdDebugLog::log("MEM", "epub-page free=%u largest=%u minEver=%u", (unsigned)ESP.getFreeHeap(),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)ESP.getMinFreeHeap());
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  // estimatedTotalPages (not pageCount): while a giant spine is still building, pageCount is only
  // the build watermark, and saving it as the chapter total would teleport the reader on resume.
  // estimatedTotalPages returns the smoothed real total. maybeSaveProgress dedups internally.
  maybeSaveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages());
  // Fold any still-open visible segment into the page's accumulated dwell. The forward-turn /
  // menu / dictionary / exit paths reset pageShownAtMs to 0 first; this only fires for
  // transitions that don't (e.g. a backward page turn) — no double counting.
  accumulateVisibleSegment();
  pageShownAtMs = millis();
  // Re-anchor the marker dwell on a genuine new page. When returning from word-select, resume
  // instead: shift the anchor so only the pre-launch reading time counts (the time spent inside
  // word-select is excluded), giving a paused-then-resumed dwell on the same page. The idle-page
  // accumulator follows the same rule: on a genuine new page, apply the cap to the page just
  // left and reset; on a resume, keep accumulating into the same page.
  if (preserveMarkerDwell_) {
    preserveMarkerDwell_ = false;
    markerDwellStartMs = millis() - markerDwellPausedElapsedMs;
  } else {
    accountIdleExcess(currentPageVisibleMs);
    currentPageVisibleMs = 0UL;
    markerDwellStartMs = millis();
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage && !bookmarkMessageLightRefresh) {
    const StrId msgId = bookmarkMessageRemoved  ? StrId::STR_BOOKMARK_REMOVED
                        : bookmarkMessageReturn ? StrId::STR_RETURN_MARK_ADDED
                                                : StrId::STR_BOOKMARK_ADDED;
    GUI.drawPopup(renderer, I18n::getInstance().get(msgId));
  }

  if (showNoDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Re-derive the page from the saved content offset after a settings reflow.
  // Older 4/6-byte progress files retain the page-fraction fallback.
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();  // consumed; don't read cached progress again
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  // This path has no FrameBufferLoan (the reading page owns the framebuffer), so the
  // chapter's ZIP inflate window rides the heap. Skip the pre-index when the largest
  // free block can't fit the 32 KB window -- it would only fail init, retry 3x, and log
  // an error for a chapter that builds fine on open with the loan available.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < SILENT_INDEX_MIN_MAX_ALLOC) {
    LOG_DBG("ERS", "Skipping silent index of chapter %d: heap too fragmented (largest=%u)", nextSpineIndex,
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.getReaderExtraParagraphSpacing(), SETTINGS.getReaderParagraphAlignment(),
                                  viewportWidth, viewportHeight, SETTINGS.getReaderHyphenationEnabled(),
                                  SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.getReaderExtraParagraphSpacing(), SETTINGS.getReaderParagraphAlignment(),
                                     viewportWidth, viewportHeight, SETTINGS.getReaderHyphenationEnabled(),
                                     SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::launchKoSync(bool sleepWhenDone, SyncScope scope) {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return false;
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  // Nothrow; see launchKOReaderSync() — the epub is already released by this point.
  auto sync = makeUniqueNoThrow<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex, sleepWhenDone, scope);
  if (!sync) {
    LOG_ERR("KOSync", "OOM: KOReaderSyncActivity; returning home");
    activityManager.goHome();
    return true;
  }
  activityManager.replaceActivity(std::move(sync));
  return true;
}

uint32_t EpubReaderActivity::readingTotalSeconds() const {
  // The book's odometer (readingStats.totalReadingSeconds, which already includes this session's
  // *committed* checkpoints) plus only the not-yet-committed effective remainder of the live
  // session. Mirror commitReadingTime()'s accounting so the count matches the stats odometer:
  // exclude idle-page excess, and exclude the part already folded into readingStats (no double-
  // count). onPause/onResume keep sessionStartMs free of sub-screen time, so the delta is reading.
  uint32_t totalNow = readingStats.totalReadingSeconds;
  if (sessionStartMs > 0) {
    const uint32_t sessionSecs = static_cast<uint32_t>((millis() - sessionStartMs) / 1000UL);
    const uint32_t effectiveSecs = (sessionSecs > sessionIdleExcessSecs) ? (sessionSecs - sessionIdleExcessSecs) : 0;
    if (effectiveSecs > sessionCommittedSecs) {
      totalNow += effectiveSecs - sessionCommittedSecs;
    }
  }
  return totalNow;
}

uint32_t EpubReaderActivity::readingSecondsSinceLastSync() const {
  const uint32_t totalNow = readingTotalSeconds();
  return totalNow > readingStats.lastSyncReadingSeconds ? totalNow - readingStats.lastSyncReadingSeconds : 0;
}

bool EpubReaderActivity::syncPromptThresholdReached() const {
  constexpr size_t kCount = sizeof(CrossPointSettings::SYNC_PROMPT_MINUTES) / sizeof(uint8_t);
  const uint8_t idx = SETTINGS.syncPromptMinutesIdx < kCount ? SETTINGS.syncPromptMinutesIdx : 0;
  const uint32_t thresholdMinutes = CrossPointSettings::SYNC_PROMPT_MINUTES[idx];
  // Gate on reading since the LATER of the last successful sync and the last prompt-Skip, so a
  // Skip defers the next prompt by a full interval rather than re-firing while still over the
  // sync gate. A real sync (lastSyncReadingSeconds) supersedes a stale skip via the max().
  const uint32_t totalNow = readingTotalSeconds();
  const uint32_t ref = std::max(readingStats.lastSyncReadingSeconds, readingStats.lastSyncPromptSkipSeconds);
  const uint32_t sinceRef = totalNow > ref ? totalNow - ref : 0;
  return sinceRef >= thresholdMinutes * 60UL;
}

void EpubReaderActivity::recordSyncPromptSkip() {
  // Stamp the odometer at the moment of Skip and persist, so the deferral survives a
  // Skip-then-sleep then a later open (the open prompt reloads stats from SD).
  readingStats.lastSyncPromptSkipSeconds = readingTotalSeconds();
  if (epub) {
    readingStats.save(epub->getCachePath());
  }
}

bool EpubReaderActivity::onManualSleepRequested() {
  // Opt-in, reader page only, pointless without credentials, and only past the reading threshold.
  if (!SETTINGS.syncPromptOnSleep || !epub || !KOREADER_STORE.hasCredentials() || !syncPromptThresholdReached()) {
    return false;  // let the main loop sleep normally
  }

  // Take over the gesture: ask Sync / Skip / Cancel before sleeping. The reader stays on the
  // stack and resumes to run this result handler after the prompt is dismissed.
  //   Cancel (Back)  -> abort the sleep, stay reading
  //   Skip   (Left)  -> sleep now, no sync
  //   Sync   (Right) -> sync then deep-sleep on success
  startActivityForResultNoThrow<SleepSyncPromptActivity>(
      [this](const ActivityResult& res) {
        if (res.isCancelled) {
          // Cancel: don't sleep. Swallow the answering button's release so it
          // doesn't bleed into a page turn / Back on the resumed reader.
          suppressPageTurnUntilRelease_ = true;
          ignoreBackUntilRelease = true;
          return;
        }
        const auto* menu = std::get_if<MenuResult>(&res.data);
        if (menu && menu->action == SleepSyncPromptActivity::ACTION_SYNC) {
          // Sync -> hand off; KOReaderSyncActivity deep-sleeps on success (sleepWhenDone).
          // If the pre-sync save failed, don't strand the user awake — sleep anyway.
          if (!launchKoSync(/*sleepWhenDone=*/true)) {
            APP_STATE.requestManualSleep = true;
          }
          return;
        }
        // Skip -> sleep now, no sync. Defer the next prompt by a full
        // interval (survives this sleep + the next open).
        recordSyncPromptSkip();
        APP_STATE.requestManualSleep = true;
      },
      renderer, mappedInput, tr(STR_SYNC_BEFORE_SLEEP), tr(STR_SYNC_BEFORE_SLEEP_BODY));
  return true;
}

void EpubReaderActivity::showOpenSyncPrompt() {
  // Reader stays on the stack and resumes to run this handler after the prompt is dismissed.
  startActivityForResultNoThrow<ConfirmationActivity>(
      [this](const ActivityResult& res) {
        if (res.isCancelled) {
          // Skip: keep reading. Defer the next prompt by a full interval so it doesn't
          // re-fire on the next open while still over the sync gate.
          recordSyncPromptSkip();
          // Swallow the answering button's release so it doesn't bleed into a page turn /
          // Back on the resumed reader.
          suppressPageTurnUntilRelease_ = true;
          ignoreBackUntilRelease = true;
          return;
        }
        // Sync → let the user pick what to sync first (same as the reader-menu Sync
        // path), then launch the matching scope and return to the reader (no sleep).
        // On Back at the scope picker, stay reading without launching.
        startActivityForResultNoThrow<SyncScopeSelectionActivity>(
            [this](const ActivityResult& scopeRes) {
              if (scopeRes.isCancelled) return;
              if (const auto* scopeResult = std::get_if<SyncScopeResult>(&scopeRes.data)) {
                launchKoSync(/*sleepWhenDone=*/false, scopeResult->scope);
              }
            },
            renderer, mappedInput);
      },
      renderer, mappedInput, tr(STR_SYNC_BEFORE_READING), tr(STR_SYNC_BEFORE_READING_BODY), tr(STR_SKIP), tr(STR_SYNC));
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  std::optional<uint32_t> offset;
  if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  const bool ok = EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
  if (ok) {
    // Track what is now persisted so the debounce knows when the on-SD position is current.
    lastSavedSpine_ = spineIndex;
    lastSavedPage_ = currentPage;
    turnsSinceProgressSave_ = 0;
  }
  return ok;
}

void EpubReaderActivity::maybeSaveProgress(int spineIndex, int currentPage, int pageCount) {
  // Unchanged position (e.g. a resume re-render after the position was already saved): nothing to do.
  if (spineIndex == lastSavedSpine_ && currentPage == lastSavedPage_) return;
  const uint8_t idx = SETTINGS.progressSaveIntervalIdx;
  constexpr size_t kCount = sizeof(CrossPointSettings::PROGRESS_SAVE_PAGES) / sizeof(uint16_t);
  const uint16_t interval = CrossPointSettings::PROGRESS_SAVE_PAGES[idx < kCount ? idx : 0];
  // ++turns then compare: interval 1 saves every change (legacy behaviour). A resume re-render
  // while already dirty can over-count by one, which only makes the next save happen sooner —
  // the safe direction (less potential page loss, marginally more wear).
  if (++turnsSinceProgressSave_ >= interval) {
    saveProgress(spineIndex, currentPage, pageCount);  // resets the counter + last-saved tracking
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  lastPageUsedGrayscale = false;  // cleared here; set below if grayscale pass runs

  // The image pixel-cache RAM slot (#2611) lives for exactly one page render (it
  // feeds the BW double-refresh and every grayscale band pass); release it on
  // every exit path so nothing stays resident across page turns.
  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  // Propagate the image-dither choice (Display > Image Dither) to the renderer
  // so ImageBlock picks the dither field. EPUB images decode in JPEG blocks, so
  // error-diffusion isn't possible there — imageDitherBlueNoise() maps it to
  // blue noise (the best ordered field) for EPUB.
  renderer.setImageDitherMode(SETTINGS.imageDither);

  // Text AA mode (X4): Off = 1-bit images + black text; Antialiased = grey images +
  // grey (AA) text; Sharp = grey images + true-black text. Images are 1-bit only in
  // Off mode; text contributes grey to the grayscale planes only in Antialiased mode.
  const uint8_t aaMode = SETTINGS.textAntiAliasing;
  renderer.setOneBitImages(renderer.isX3() || aaMode == CrossPointSettings::TEXT_AA_OFF);
  renderer.setTextAntiAlias(aaMode == CrossPointSettings::TEXT_AA_ANTIALIASED);

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  // Force special handling for pages with images when anti-aliasing is on.
  // Not needed on X3: images render as a 1-bit halftone that survives the
  // grayscale text-AA pass untouched (gc bb cell preserves it), so the
  // image-blanking double-refresh dance below would only add a visible flash.
  // Grayscale (4-level) images must render via the grayscale pass on X4 even when
  // text anti-aliasing is OFF — otherwise the image falls back to 1-bit BW and
  // looks too dark. Text AA only governs whether *text* is antialiased. (X3 uses
  // a 1-bit image halftone, so it doesn't need the 4-level gray pass.)
  // 4-level grayscale images only exist when text AA is on (AA off => images are
  // 1-bit, drawn in the BW frame, needing no grayscale pass or blanking dance).
  // A "large" image (both dimensions >= this) is a real figure/photo. Small inline
  // icons, emoji, and thin dividers fall below it and must NOT force the next-page
  // ghost-clear HALF_REFRESH (their residue is negligible). Rendering still uses the
  // unconditional hasImages() path so even tiny icons draw correctly; only the
  // refresh-cadence forcing and the bookmark light-refresh gate on the size check.
  static constexpr int16_t IMAGE_LARGE_MIN_PX = 64;
  const bool hasLargeImage = page->hasLargeImages(IMAGE_LARGE_MIN_PX);

  lastPageHadImages = hasLargeImage;  // gates the bookmark light-refresh (see header)
  const bool grayImages = page->hasImages() && !renderer.isX3() && aaMode != CrossPointSettings::TEXT_AA_OFF;
  // Antialiased always runs the gray pass (text AA, even text-only pages). Sharp runs
  // it only for image pages — pure-text Sharp pages stay single-pass solid black.
  const bool doGrayscalePass = (aaMode == CrossPointSettings::TEXT_AA_ANTIALIASED) || grayImages;
  // Any grayscale image page must use the FAST_REFRESH blanking dance below
  // (even with text AA off): a HALF/FULL refresh sets the e-ink particles too
  // firmly for the following grayscale LUT to adjust, which washed image pages
  // out to near-white. So gate this on grayImages, not on text AA.
  bool imagePageWithAA = grayImages;

  // Whether any image on this page still needs decoding — gates the placeholder
  // pre-pass below (upstream #1003). Grayscale/refresh cadence still keys off
  // grayImages / hasLargeImage above.
  const bool pageHasImagesNeedingDecode = page->hasImages() && page->hasImagesNeedingDecode(renderer);

  // Grayscale-pass content selector (perf; upstream #2393's Page::renderImages()).
  // Antialiased: text contributes grey, so render the whole page each strip.
  // Sharp / images-only: text is already solid-black in the BW frame, so the
  // grayscale strips only need the images — skip the redundant text render.
  auto renderGrayscalePass = [&]() {
    if (aaMode == CrossPointSettings::TEXT_AA_ANTIALIASED) {
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    }
  };

  // No automatic ghost-clear flash on image page turns — the power-button manual
  // refresh (HALF clear + re-render) is the ghost-clear tool when the user wants it.

  // Placeholder pre-pass (upstream #1003): show text + image-placeholder boxes with a
  // FAST_REFRESH immediately so the reader isn't blank while images decode. clearScreen()
  // wipes the framebuffer; the normal render/grayscale flow below then repaints for real.
  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Port of upstream #2747 (X4 AA image path only; X3 never reaches here since
      // imagePageWithAA is false). When a full scrub is due -- returning from
      // KOReader sync / wake / cold boot (pagesUntilFullRefresh == FORCE_FULL, set
      // by ReaderActivity::initialRefreshCountdown), or the periodic maintenance
      // page -- the double-FAST dance below would otherwise run straight over the
      // retained frame (e.g. the sync "Progress found" screen), leaving the old UI
      // mixed under the image. Lay a clean HALF base first. The gate mirrors
      // displayWithRefreshCycle's maintenance check, so the "Refresh: Never"
      // (DISABLED) sentinel is excluded and does not force a base pass.
      const bool cleanImageBasePending =
          pagesUntilFullRefresh != CrossPointSettings::REFRESH_COUNTDOWN_DISABLED && pagesUntilFullRefresh <= 1;
      if (cleanImageBasePending) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      // No image bounding box (e.g. full-page image): still use FAST_REFRESH, not
      // HALF — a HALF/FULL refresh sets the e-ink particles too firmly for the
      // grayscale pass that follows, washing the page out to near-white.
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue. Only large images leave enough residue to warrant
    // this — small icons/emoji skip it (no needless refresh on the next page).
    if (hasLargeImage) {
      pagesUntilFullRefresh = CrossPointSettings::REFRESH_COUNTDOWN_FORCE_FULL;
    }
  } else {
    // Full-refresh pages double as reading-time checkpoints: the 1-2s HALF_REFRESH
    // masks the stats SD writes. loop() (main task) performs the actual commit.
    if (pagesUntilFullRefresh <= 1) {
      statsCheckpointPending = true;
    }
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    // X3 halftone image residue: 1-bit halftone dots leave charge that FAST_REFRESH
    // can't fully clear on the next page. Force HALF on the next page to drive every
    // pixel to its target — same fix as the X4 grayscale residue path above.
    if (hasLargeImage && renderer.isX3()) {
      pagesUntilFullRefresh = CrossPointSettings::REFRESH_COUNTDOWN_FORCE_FULL;
    }
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (doGrayscalePass && renderer.supportsStripGrayscale()) {
    lastPageUsedGrayscale = true;
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (doGrayscalePass) {
      lastPageUsedGrayscale = true;
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      renderer.storeBwBuffer();
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No anti-aliasing: BW frame already displayed above, no grayscale to
      // render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const float bmPageProgress = (section && section->pageCount > 0)
                                   ? static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount)
                                   : 0.0f;
  const bool hasSection = section && section->pageCount > 0;
  // Split by type so a page can show both the bookmark tab and the quote glyph.
  const bool pointBookmarked = hasSection && BOOKMARKS.hasPointBookmarkForPage(static_cast<uint16_t>(currentSpineIndex),
                                                                               bmPageProgress, section->pageCount);
  const bool quoted = hasSection && BOOKMARKS.hasQuoteForPage(static_cast<uint16_t>(currentSpineIndex), bmPageProgress,
                                                              section->pageCount);
  const bool returnMark = pointBookmarked && BOOKMARKS.isReturnMarkForPage(static_cast<uint16_t>(currentSpineIndex),
                                                                           bmPageProgress, section->pageCount);
  // pageCountEstimated = the section is still building, so the page total is a smoothed estimate
  // (drawStatusBar prefixes it with "~"). Matches upstream's building indicator.
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, pointBookmarked,
                    returnMark, quoted, section && section->isBuilding());
}

void EpubReaderActivity::lightStatusBarRefresh() {
  const int barH = static_cast<int>(UITheme::getInstance().getStatusBarHeight());
  if (barH == 0) return;  // no status bar → no bookmark indicator to update

  if (lastPageHadImages) {
    // Image page: a full-frame FAST drives the SSD1677 grayscale LUT and darkens the
    // image on every toggle. Re-render the whole page (its own stable image refresh)
    // so the status bar / tab updates without disturbing the image.
    requestUpdate();
    return;
  }

  RenderLock lock(*this);

  int orientedTop, orientedRight, orientedBottom, orientedLeft;
  renderer.getOrientedViewableTRBL(&orientedTop, &orientedRight, &orientedBottom, &orientedLeft);

  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();

  // Blank ONLY the status-bar region (from its top edge down). The bookmark icon sits
  // inside the bar (top edge at bar_top, 14px tall), so blanking the bar clears a
  // removed tab. We deliberately do NOT extend the blank above the bar (the old
  // BOOKMARK_TAB_EXTRA=20 did, erasing the page's bottom text line — visible as cut
  // text, worst in landscape). The only above-bar content is the progress text, which
  // does not change on a bookmark toggle, so it needs no blank/redraw region here.
  int stripY = sh - barH - orientedBottom;
  if (stripY < 0) stripY = 0;
  const int stripH = sh - stripY;
  renderer.fillRect(0, stripY, sw, stripH, false);
  renderStatusBar();

  // Push the WHOLE current framebuffer with a full-frame FAST refresh, NOT a windowed
  // sub-rect: on this X4 (SSD1677, single-buffer) a windowed FAST exposes stale
  // RED-RAM in the untouched page area, reverting the panel to the *previous* rendered
  // page. A full-frame FAST drives the entire current framebuffer (current page + the
  // updated tab) — correct page, ~0.4s, no page re-render, no 1.5s HALF. AA text
  // reverts to 1-bit until the next page turn re-applies grayscale.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  lastPageUsedGrayscale = false;
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::addBookmark(bool returnMark, bool lightRefresh) {
  if (!section || !epub) {
    return;
  }

  int pageCount;
  int currentPage;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }
  if (pageCount == 0) return;

  const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
  const float progress = static_cast<float>(currentPage) / static_cast<float>(pageCount);

  const char* chapterTitle = nullptr;
  std::string titleStr;
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) {
    titleStr = epub->getTocItem(tocIndex).title;
    chapterTitle = titleStr.c_str();
  }

  uint16_t paragraphIndex = UINT16_MAX;
  if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
    paragraphIndex = *pIdx;
  }

  char snippet[BOOKMARK_SNIPPET_MAX] = {};
  if (auto page = section->loadPage(section->currentPage)) {
    buildBookmarkSnippet(*page, snippet, sizeof(snippet));
  }

  LOG_DBG("ERS", "Adding bookmark at spine %d, page %d", currentSpineIndex, currentPage);
  const auto addResult =
      BOOKMARKS.addBookmark(spine, progress, pageCount, chapterTitle, paragraphIndex, snippet, returnMark, currentPage);
  if (addResult == BookmarkStore::AddResult::Added) {
    if (lightRefresh) {
      // Interactive toggle: update only the status-bar strip (image untouched).
      // Also set showBookmarkMessage + bookmarkMessageTime as a debounce timer so
      // hold-Confirm doesn't re-fire every loop tick while the button is held.
      showBookmarkMessage = true;
      bookmarkMessageLightRefresh = true;
      bookmarkMessageTime = millis();
      lightStatusBarRefresh();
    } else {
      // Navigation return-mark or other full-render path: show the timed popup
      // and schedule a full re-render as before.
      showBookmarkMessage = true;
      bookmarkMessageRemoved = false;
      bookmarkMessageReturn = returnMark;
      bookmarkMessageTime = millis();
      requestUpdate();
    }
  } else {
    LOG_ERR("ERS", "Bookmark limit reached");
    // Tell the user (a deliberate add only — auto-dropped return marks stay silent).
    if (!returnMark) {
      RenderLock lock(*this);
      GUI.drawPopup(renderer, tr(STR_MARK_LIMIT));  // refreshes internally (BaseTheme.cpp:803)
      delay(900);
      requestUpdate();
    }
  }
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
