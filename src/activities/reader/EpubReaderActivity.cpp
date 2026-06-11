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
#include <Serialization.h>
#include <ZipFile.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../settings/DictionarySelectActivity.h"
#include "BookStatsActivity.h"
#include "BookmarkStore.h"
#include "ReaderOptionsActivity.h"
#include "ReaderSettingsIO.h"
#include "SdCardFontSystem.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "GlobalReadingStats.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "LookedUpWordsActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingTimeHistory.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "activities/util/ConfirmationActivity.h"
#include "util/Dictionary.h"
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

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Bookmark + tombstone files are keyed by crc32 of the epub path, so re-key them too,
  // otherwise the moved book loses its bookmarks.
  BookmarkStore::relocateForFilePath(srcPath, dstPath, "epub");

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

bool isSnippetWhitespace(const std::string& word) {
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
    if (!line.getBlock()) continue;
    const auto& words = line.getBlock()->getWords();
    for (const auto& word : words) {
      if (isSnippetWhitespace(word)) continue;
      const size_t separatorLen = len > 0 ? 1 : 0;
      const size_t wordLen = word.size();
      if (len + separatorLen + wordLen >= outSize) return;
      if (separatorLen > 0) out[len++] = ' ';
      memcpy(out + len, word.c_str(), wordLen);
      len += wordLen;
      out[len] = '\0';
    }
  }
}

// Persists `sessionSecs` of reading time to both per-book and global stats on exit.
// `dated` is true when the RTC (X3 only) supplied a calendar date for this session;
// dated sessions feed the weekly/monthly/yearly/heatmap history, undated ones (X4,
// no clock) fall back to `unattributedSeconds` — counted in totals but not dated.
void recordReadingSession(const std::string& cachePath, BookReadingStats& bookStats, uint32_t sessionSecs,
                          bool dated, uint16_t year, uint8_t month, uint8_t day, uint8_t dayOfWeek,
                          uint8_t hour, uint8_t minute) {
  if (sessionSecs > 0) {
    bookStats.totalReadingSeconds += sessionSecs;
    if (dated) {
      // "Last read on ..." stamp for the Vega hero card -- only set on dated
      // (X3 + RTC) sessions, encoded the same way as ReadingTimeHistory's
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

  epub->setupCacheDir();

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
      bookOverride.fontSize = SETTINGS.fontSize;
      bookOverride.lineSpacing = SETTINGS.lineSpacing;
      bookOverride.paragraphAlignment = SETTINGS.paragraphAlignment;
      bookOverride.hyphenationEnabled = SETTINGS.hyphenationEnabled;
      bookOverride.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
      static_assert(sizeof(bookOverride.sdFontFamilyName) == sizeof(SETTINGS.sdFontFamilyName),
                    "sdFontFamilyName size mismatch");
      strncpy(bookOverride.sdFontFamilyName, SETTINGS.sdFontFamilyName,
              sizeof(bookOverride.sdFontFamilyName) - 1);
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
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
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
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
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
  sessionIdleExcessSecs = 0;

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  if (epub && sessionStartMs > 0) {
    // Account the page being viewed at exit (its final dwell) before totalling.
    if (pageShownAtMs > 0) accountIdleExcess(millis() - pageShownAtMs);

    const uint32_t sessionSecs = static_cast<uint32_t>((millis() - sessionStartMs) / 1000UL);
    // Subtract idle-page excess so leaving the device on a page doesn't inflate reading
    // time. With the cap Off, sessionIdleExcessSecs is 0 and this equals wall-clock.
    const uint32_t effectiveSecs = (sessionSecs > sessionIdleExcessSecs) ? (sessionSecs - sessionIdleExcessSecs) : 0;

    // Determine effective minimum session threshold: per-book override wins unless
    // it is set to MIN_SESSION_USE_GLOBAL, in which case fall back to global setting.
    const auto& ov = SETTINGS.getReaderOverride();
    const uint8_t thresholdIdx =
        (ov.active && ov.minSessionMinutes != CrossPointSettings::ReaderOverride::MIN_SESSION_USE_GLOBAL)
            ? ov.minSessionMinutes
            : SETTINGS.minSessionMinutes;
    constexpr size_t kMinSessCount = sizeof(CrossPointSettings::MIN_SESSION_SECONDS) / sizeof(uint16_t);
    const uint32_t thresholdSecs =
        (thresholdIdx < kMinSessCount) ? CrossPointSettings::MIN_SESSION_SECONDS[thresholdIdx] : 0;

    if (effectiveSecs >= thresholdSecs) {
      // Use the local calendar day (RTC raw date + SETTINGS.clockUtcOffsetQ), not the
      // RTC's raw date -- a session that starts just after local midnight must be
      // attributed to "today", not the RTC's still-previous UTC-ish day, or the
      // weekly/monthly/yearly/heatmap history buckets it under the wrong date.
      uint8_t dayOfWeek = 0, day = 0, month = 0, hour = 0, minute = 0;
      uint16_t year = 0;
      const bool dated = halClock.isAvailable() &&
                         halClock.getLocalDateTime(SETTINGS.clockUtcOffsetQ, dayOfWeek, day, month, year, hour, minute);
      recordReadingSession(epub->getCachePath(), readingStats, effectiveSecs, dated, year, month, day, dayOfWeek,
                           hour, minute);
    }

    sessionStartMs = 0UL;
    pageShownAtMs = 0UL;
  }

  // Restore global render settings view for home screen / settings / TXT reader.
  SETTINGS.clearReaderOverride();
  // Reload any SD-card font back to the global size so the home/library UI (which
  // does not re-sync the SD font itself) matches the global font size again.
  sdFontSystem.ensureLoaded(renderer);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // Restore the global-default invariant for non-reader UI.
  APP_STATE.activeOrientation = SETTINGS.orientation;

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  BOOKMARKS.unload();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

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
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
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

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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

  // Long press BACK (1s+) goes to file selection
  if (!suppressBack && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (!suppressBack && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt, fromSide] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Long-press behavior is configured separately for side vs front buttons.
  const uint8_t lpBehavior = fromSide ? SETTINGS.sideLongPressButtonBehavior : SETTINGS.longPressButtonBehavior;

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
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
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && lpBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered
            ? (APP_STATE.activeOrientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
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
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

  // Extract the folder name from the active dictionary path for display in the menu.
  // Path format: /dictionary/<folder>/<stem> — we want <folder>.
  std::string activeDictName;
  {
    const std::string rawDictPath = Dictionary::readDictPath(epub->getCachePath().c_str());
    if (rawDictPath.empty()) {
      activeDictName = tr(STR_DICT_NONE);
    } else {
      const size_t lastSlash = rawDictPath.rfind('/');
      if (lastSlash != std::string::npos && lastSlash > 0) {
        const size_t prevSlash = rawDictPath.rfind('/', lastSlash - 1);
        activeDictName = (prevSlash != std::string::npos) ? rawDictPath.substr(prevSlash + 1, lastSlash - prevSlash - 1)
                                                          : rawDictPath.substr(0, lastSlash);
      } else {
        activeDictName = rawDictPath;
      }
    }
  }

  if (pageShownAtMs > 0) accountIdleExcess(millis() - pageShownAtMs);
  pageShownAtMs = 0UL;
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
          APP_STATE.activeOrientation,
          !currentPageFootnotes.empty(),
          Dictionary::exists(epub->getCachePath().c_str()), std::move(activeDictName)),
      [this](const ActivityResult& result) {
        // Always apply orientation change even if the menu was cancelled
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

void EpubReaderActivity::openWordSelect(bool framebufferContainsPage) {
  auto pageForLookup = section ? section->loadPageFromSectionFile() : nullptr;
  if (!pageForLookup) {
    requestUpdate();
    return;
  }
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  // Bottom reserved-area height (matches renderContents() at line 695-703). The
  // word-select activity uses this to clear exactly the strip we drew the
  // status-bar / auto-turn label into, so its first frame matches the menu
  // path which wipes everything via clearScreen + page->render.
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const int reservedBottomHeight =
      (automaticPageTurnActive &&
       (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()))
          ? std::max(
                SETTINGS.screenMargin,
                static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin))
          : std::max(SETTINGS.screenMargin, statusBarHeight);
  std::string nextPageFirstWord;
  if (section && section->currentPage < section->pageCount - 1) {
    int savedPage = section->currentPage;
    section->currentPage = savedPage + 1;
    auto nextPage = section->loadPageFromSectionFile();
    section->currentPage = savedPage;
    if (nextPage && !nextPage->elements.empty()) {
      const auto it = std::find_if(nextPage->elements.begin(), nextPage->elements.end(),
                                   [](const auto& el) { return el->getTag() == TAG_PageLine; });
      if (it != nextPage->elements.end()) {
        const auto* firstLine = static_cast<const PageLine*>(it->get());
        if (firstLine->getBlock() && !firstLine->getBlock()->getWords().empty()) {
          nextPageFirstWord = firstLine->getBlock()->getWords().front();
        }
      }
    }
  }
  const std::string bookCachePath = epub->getCachePath();
  if (pageShownAtMs > 0) accountIdleExcess(millis() - pageShownAtMs);
  pageShownAtMs = 0UL;
  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(
                             renderer, mappedInput, std::move(pageForLookup), orientedMarginLeft, orientedMarginTop,
                             bookCachePath, nextPageFirstWord, framebufferContainsPage, reservedBottomHeight),
                         [this](const ActivityResult&) {
                           ignoreBackUntilRelease = true;
                           requestUpdate();
                         });
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      if (currentSpineIndex != sync.spineIndex || (section && section->currentPage != sync.page)) {
        RenderLock lock(*this);
        currentSpineIndex = sync.spineIndex;
        nextPageNumber = sync.page;
        section.reset();
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      if (!section || section->pageCount == 0) break;
      const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      if (BOOKMARKS.hasBookmarkForPage(spine, progress, section->pageCount)) {
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
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto chapterResult = std::get<ChapterResult>(result.data);

              auto doNavigate = [this, chapterResult]() {
                RenderLock lock(*this);
                currentSpineIndex = chapterResult.spineIndex;
                pendingAnchor = chapterResult.anchor;
                nextPageNumber = 0;
                section.reset();
              };

              if (section && section->pageCount > 0 && chapterResult.spineIndex != currentSpineIndex) {
                const float bmProgress =
                    static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
                if (!BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), bmProgress,
                                                  section->pageCount)) {
                  startActivityForResult(
                      std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                             tr(STR_CONFIRM_ADD_RETURN_MARK), ""),
                      [this, doNavigate](const ActivityResult& confirmResult) {
                        if (!confirmResult.isCancelled) {
                          addBookmark(/*returnMark=*/true, /*lightRefresh=*/true);
                        }
                        doNavigate();
                      });
                  return;
                }
              }
              doNavigate();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this, initialPercent](const ActivityResult& result) {
            if (!result.isCancelled) {
              const int targetPercent = clampPercent(std::get<PercentResult>(result.data).percent);

              auto doNavigate = [this, targetPercent]() { jumpToPercent(targetPercent); };

              if (section && section->pageCount > 0 && targetPercent != initialPercent) {
                const float bmProgress =
                    static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
                if (!BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), bmProgress,
                                                  section->pageCount)) {
                  startActivityForResult(
                      std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                             tr(STR_CONFIRM_ADD_RETURN_MARK), ""),
                      [this, doNavigate](const ActivityResult& confirmResult) {
                        if (!confirmResult.isCancelled) {
                          addBookmark(/*returnMark=*/true, /*lightRefresh=*/true);
                        }
                        doNavigate();
                      });
                  return;
                }
              }
              doNavigate();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
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
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_CONFIRM_DELETE_CACHE), ""),
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
          });
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
          return;
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

        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP: {
      // Menu activity rendered over the page; the framebuffer no longer
      // matches what DictionaryWordSelectActivity expects.
      openWordSelect(/*framebufferContainsPage=*/false);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP_HISTORY: {
      startActivityForResult(std::make_unique<LookedUpWordsActivity>(renderer, mappedInput, epub->getCachePath()),
                             [this](const ActivityResult&) {
                               ignoreBackUntilRelease = true;
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SET_BOOK_DICTIONARY: {
      startActivityForResult(std::make_unique<DictionarySelectActivity>(renderer, mappedInput, epub->getCachePath()),
                             [this](const ActivityResult&) { openReaderMenu(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS: {
      startActivityForResult(
          std::make_unique<ReaderOptionsActivity>(renderer, mappedInput, epub->getCachePath(),
                                                  SETTINGS.getReaderOverride()),
          [this](const ActivityResult&) {
            // Re-layout: the per-book settings may have changed, so discard the
            // cached section and let render() rebuild it with the new parameters.
            RenderLock lock(*this);
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
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub->getPath()),
          [this](const ActivityResult& result) {
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
                section.reset();
              };

              if (section && section->pageCount > 0) {
                const float bmProgress =
                    static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
                if (!BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), bmProgress,
                                                  section->pageCount)) {
                  startActivityForResult(
                      std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                             tr(STR_CONFIRM_ADD_RETURN_MARK), ""),
                      [this, doNavigate](const ActivityResult& confirmResult) {
                        if (!confirmResult.isCancelled) {
                          addBookmark(/*returnMark=*/true, /*lightRefresh=*/true);
                        }
                        doNavigate();
                      });
                  return;
                }
              }
              doNavigate();
            }
          });
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
      session.elapsedSecs =
          sessionStartMs > 0 ? static_cast<uint32_t>((millis() - sessionStartMs) / 1000UL) : 0UL;
      {
        const auto& ov = SETTINGS.getReaderOverride();
        const uint8_t thresholdIdx =
            (ov.active && ov.minSessionMinutes != CrossPointSettings::ReaderOverride::MIN_SESSION_USE_GLOBAL)
                ? ov.minSessionMinutes
                : SETTINGS.minSessionMinutes;
        constexpr size_t kMinSessCount = sizeof(CrossPointSettings::MIN_SESSION_SECONDS) / sizeof(uint16_t);
        session.thresholdSecs =
            (thresholdIdx < kMinSessCount) ? CrossPointSettings::MIN_SESSION_SECONDS[thresholdIdx] : 0;
        uint8_t hour = 0, minute = 0;
        session.dated = halClock.isAvailable() &&
                        halClock.getLocalDateTime(SETTINGS.clockUtcOffsetQ, session.dayOfWeek, session.day,
                                                  session.month, session.year, hour, minute);
      }
      startActivityForResult(
          std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getCachePath(),
                                               progressPercent, session),
          [this](const ActivityResult&) {
            ignoreBackUntilRelease = true;
            requestUpdate();
          });
      break;
    }
  }
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
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::accountIdleExcess(unsigned long dwellMs) {
  const uint8_t idx = SETTINGS.pageIdleCapSeconds;
  if (idx == 0) return;  // Off — full wall-clock, no idle cap.
  constexpr size_t kCount = sizeof(CrossPointSettings::PAGE_IDLE_CAP_SECONDS) / sizeof(uint16_t);
  if (idx >= kCount) return;  // out of range (shouldn't happen) — treat as Off.
  const uint32_t capSecs = CrossPointSettings::PAGE_IDLE_CAP_SECONDS[idx];
  const uint32_t dwellSecs = static_cast<uint32_t>(dwellMs / 1000UL);
  // Only pages held past the idle threshold are capped; cap <= threshold so this can't
  // underflow.
  if (dwellSecs > CrossPointSettings::PAGE_IDLE_THRESHOLD_SECONDS) {
    sessionIdleExcessSecs += dwellSecs - capSecs;
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (pageShownAtMs > 0) {
      const unsigned long dwell = millis() - pageShownAtMs;
      // Idle-cap accounting is independent of the pace-sample outlier rejection below.
      accountIdleExcess(dwell);
      constexpr unsigned long MIN_DWELL_MS = 2000UL;
      if (dwell >= MIN_DWELL_MS) {
        const uint32_t dwellSecs = static_cast<uint32_t>(dwell / 1000UL);
        if (readingStats.avgSecondsPerForwardPage == 0 ||
            dwellSecs <= 2U * static_cast<uint32_t>(readingStats.avgSecondsPerForwardPage)) {
          readingStats.recordForwardPageRead(dwellSecs);
        }
      }
      pageShownAtMs = 0UL;
    }

    if (section->currentPage < section->pageCount - 1) {
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

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
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
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

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

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.getReaderExtraParagraphSpacing(), SETTINGS.getReaderParagraphAlignment(),
                                  viewportWidth, viewportHeight, SETTINGS.getReaderHyphenationEnabled(),
                                  SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      LOG_DBG("ERS", "Cache not found, building...");

      const Rect indexingPopup = GUI.drawPopup(renderer, tr(STR_INDEXING));

      // Fill the popup's progress bar as the chapter is parsed so a long index on a
      // big chapter no longer looks like a hang. pct is 0-100 from the parser.
      const auto popupFn = [this, indexingPopup](const int pct) {
        GUI.fillPopupProgress(renderer, indexingPopup, pct);
      };

      Section::BuildFailure buildFailure;
      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.getReaderExtraParagraphSpacing(), SETTINGS.getReaderParagraphAlignment(),
                                      viewportWidth, viewportHeight, SETTINGS.getReaderHyphenationEnabled(),
                                      SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.focusReadingEnabled,
                                      popupFn, &buildFailure)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        buildFailedSpine = currentSpineIndex;  // stop the per-frame rebuild loop
        section.reset();
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
        {
          // Phase-A overlay: buildFailure carries WHICH branch failed + the heap
          // captured AT the failure point (before createSectionFile's cleanup), so
          // this is the real number, not the post-reset heap. Line 1: reason+heap.
          // Line 2: heap floor + inflated HTML size (LowHeap diagnosis).
          char dbg[72];
          if (buildFailure.reason == Section::BuildFailure::Reason::Stream) {
            // Phase A-2: append the ZipFile sub-reason so STREAM says which exit.
            snprintf(dbg, sizeof(dbg), "[E2/STREAM:%s spine=%d heap=%u]",
                     ZipFile::streamResultTag(static_cast<ZipFile::StreamResult>(buildFailure.streamSub)),
                     currentSpineIndex, (unsigned)buildFailure.failHeap);
          } else {
            snprintf(dbg, sizeof(dbg), "[E2/%s spine=%d heap=%u]", Section::buildFailureTag(buildFailure.reason),
                     currentSpineIndex, (unsigned)buildFailure.failHeap);
          }
          renderer.drawCenteredText(UI_12_FONT_ID, 330, dbg, true);
          char dbg2[64];
          snprintf(dbg2, sizeof(dbg2), "floor=%u html=%u", (unsigned)buildFailure.floor, (unsigned)buildFailure.htmlSize);
          renderer.drawCenteredText(UI_12_FONT_ID, 355, dbg2, true);
        }
        // No renderStatusBar(): section was just reset (null) and it derefs section->.
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      buildFailedSpine = -1;  // built OK; allow this chapter again
      sectionJustRebuilt = true;  // built fresh this pass; page load must work now
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
      sectionJustRebuilt = false;  // using existing cache; one rebuild is allowed if it's stale
      buildFailedSpine = -1;       // loaded OK; allow this chapter again
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
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
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    {
      char dbg[56];
      snprintf(dbg, sizeof(dbg), "[E3 pg=%d/%d heap=%u]", section->currentPage, section->pageCount,
               (unsigned)esp_get_free_heap_size());
      renderer.drawCenteredText(UI_12_FONT_ID, 330, dbg, true);
    }
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      if (sectionJustRebuilt) {
        // The cache was just rebuilt and the page STILL won't load — rebuilding
        // again would loop forever ("stuck on Indexing"). Stop and surface it.
        LOG_ERR("ERS", "Page load failed even after rebuild - aborting (no re-index loop)");
        section->clearCache();
        section.reset();
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
        {
          char dbg[48];
          snprintf(dbg, sizeof(dbg), "[E4 spine=%d heap=%u]", currentSpineIndex, (unsigned)esp_get_free_heap_size());
          renderer.drawCenteredText(UI_12_FONT_ID, 330, dbg, true);
        }
        // No renderStatusBar(): section was just reset (null) and it derefs section->.
        renderer.displayBuffer();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }
      LOG_ERR("ERS", "Failed to load page from SD - clearing stale cache, rebuilding once");
      section->clearCache();
      section.reset();
      requestUpdate();  // rebuild once; sectionJustRebuilt guard prevents looping
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  // Catch-all: account any still-open page view before starting a new one. The
  // forward-turn / menu / dictionary / exit paths reset pageShownAtMs to 0 first, so
  // this only fires for transitions that don't (e.g. a backward page turn) — no
  // double counting.
  if (pageShownAtMs > 0) accountIdleExcess(millis() - pageShownAtMs);
  pageShownAtMs = millis();

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage && !bookmarkMessageLightRefresh) {
    const StrId msgId = bookmarkMessageRemoved   ? StrId::STR_BOOKMARK_REMOVED
                        : bookmarkMessageReturn  ? StrId::STR_RETURN_MARK_ADDED
                                                 : StrId::STR_BOOKMARK_ADDED;
    GUI.drawPopup(renderer, I18n::getInstance().get(msgId));
  }

  if (showNoDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
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

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  lastPageUsedGrayscale = false;  // cleared here; set below if grayscale pass runs

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
  const bool grayImages = page->hasImages() && !renderer.isX3() && aaMode != CrossPointSettings::TEXT_AA_OFF;
  // Antialiased always runs the gray pass (text AA, even text-only pages). Sharp runs
  // it only for image pages — pure-text Sharp pages stay single-pass solid black.
  const bool doGrayscalePass = (aaMode == CrossPointSettings::TEXT_AA_ANTIALIASED) || grayImages;
  // Any grayscale image page must use the FAST_REFRESH blanking dance below
  // (even with text AA off): a HALF/FULL refresh sets the e-ink particles too
  // firmly for the following grayscale LUT to adjust, which washed image pages
  // out to near-white. So gate this on grayImages, not on text AA.
  bool imagePageWithAA = grayImages;

  // No automatic ghost-clear flash on image page turns — the power-button manual
  // refresh (HALF clear + re-render) is the ghost-clear tool when the user wants it.

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
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    // X3 halftone image residue: 1-bit halftone dots leave charge that FAST_REFRESH
    // can't fully clear on the next page. Force HALF on the next page to drive every
    // pixel to its target — same fix as the X4 grayscale residue path above.
    if (page->hasImages() && renderer.isX3()) {
      pagesUntilFullRefresh = 1;
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
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
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
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
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
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
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
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
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

  const float bmPageProgress =
      (section && section->pageCount > 0)
          ? static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount)
          : 0.0f;
  const bool bookmarked =
      section && section->pageCount > 0 &&
      BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), bmPageProgress, section->pageCount);
  const bool returnMark =
      bookmarked &&
      BOOKMARKS.isReturnMarkForPage(static_cast<uint16_t>(currentSpineIndex), bmPageProgress, section->pageCount);
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, bookmarked,
                    returnMark);
}

void EpubReaderActivity::lightStatusBarRefresh() {
  const int barH = static_cast<int>(UITheme::getInstance().getStatusBarHeight());
  if (barH == 0) return;  // no status bar → no bookmark indicator to update

  RenderLock lock(*this);

  int orientedTop, orientedRight, orientedBottom, orientedLeft;
  renderer.getOrientedViewableTRBL(&orientedTop, &orientedRight, &orientedBottom, &orientedLeft);

  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();

  // Cover the full bar height plus the 14px bookmark tab that extends above it,
  // with a small extra margin. Clamp to screen top.
  constexpr int BOOKMARK_TAB_EXTRA = 20;
  int stripY = sh - barH - orientedBottom - BOOKMARK_TAB_EXTRA;
  if (stripY < 0) stripY = 0;
  const int stripH = sh - stripY;

  // Blank the strip to white so a removed bookmark tab doesn't ghost, then
  // redraw the bar (which draws the tab only if still bookmarked).
  renderer.fillRect(0, stripY, sw, stripH, false);
  renderStatusBar();

  if (lastPageUsedGrayscale) {
    // AA/grayscale image page: a windowed FAST_REFRESH scans the full SSD1677
    // panel and drives grayscale particles even for "no-change" pixels, causing
    // progressive darkening on repeated toggles. Instead push the full BW
    // framebuffer once — the image reverts from AA to 1-bit (still readable)
    // until the next page turn re-applies grayscale. Tab appears immediately.
    // No layout recompute, no image re-decode — just a single FAST push of
    // already-rendered 1-bit content.
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    // Page no longer shows AA; treat subsequent toggles as non-AA.
    lastPageUsedGrayscale = false;
    return;
  }

  // Non-AA page: windowed sub-rectangle push — image area is pure 1-bit and
  // can tolerate repeated FAST passes without charge accumulation.
  renderer.displayWindowRegion(0, stripY, sw, stripH);
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
    pageCount = section->pageCount;
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
  if (auto page = section->loadPageFromSectionFile()) {
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
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
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
  const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
