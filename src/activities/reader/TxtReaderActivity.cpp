#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

#include <cstring>

#include "../../SdCardFontSystem.h"
#include "../../TxtBookmarkStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "ReaderOptionsActivity.h"
#include "ReaderSettingsIO.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "TxtReaderBookmarksActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 3;          // Increment when cache format changes

// Per-book orientation cache file (.crosspoint/<hash>/orientation.bin).
// Byte 0 = version, byte 1 = orientation value. Mirrors the EPUB reader.
constexpr uint8_t ORIENTATION_FILE_VERSION = 1;

// Auto page-turn rates (pages per minute), indexed by the menu option.
// Index 0 is "off"; the rest mirror EpubReaderActivity's PAGE_TURN_RATES.
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  // If the book was moved/renamed outside the firmware, re-key its orphaned cache dir
  // (progress, stats) before setupCacheDir() creates a fresh empty one.
  tryRecoverBookCache(txt->getPath());
  txt->setupCacheDir();
  ensureCacheContentId(txt->getPath(), txt->getCachePath());

  // Load this book's saved orientation; fall back to the global default if none.
  loadOrientation();
  ReaderUtils::applyOrientation(renderer, APP_STATE.activeOrientation);

  // Cache bookmarked pages for the status-bar indicator.
  reloadBookmarkPages();

  // Load per-book reader settings; seed from current globals on first open. This
  // makes Reader Options (font/margin/spacing) override-aware just like EPUB.
  {
    CrossPointSettings::ReaderOverride bookOverride;
    if (!ReaderSettingsIO::load(txt->getCachePath(), bookOverride)) {
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
      if (!ReaderSettingsIO::write(txt->getCachePath(), bookOverride)) {
        LOG_ERR("TRS", "Failed to seed per-book reader settings");
      }
    }
    SETTINGS.setReaderOverride(bookOverride);
  }

  // Reload any SD-card font at this book's (override) size before the first layout.
  sdFontSystem.ensureLoaded(renderer);

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onResume() {
  // Adopt an orientation parked by the control-center tile, which cannot turn the
  // renderer while its sheet is up. applyOrientation() reflows and saves per file.
  if (APP_STATE.pendingOrientation != CrossPointState::NO_ORIENTATION_REQUEST) {
    applyOrientation(APP_STATE.pendingOrientation);
  }
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Drop this book's per-book reader settings override so the rest of the UI
  // uses the global defaults again.
  SETTINGS.clearReaderOverride();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.activeOrientation = SETTINGS.orientation;

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  // Suppress Back bleed-through after a sub-activity (menu, options, bookmarks)
  // exits on Back. Capture the flag BEFORE clearing it so the release frame
  // itself is gated.
  const bool suppressBack = ignoreBackUntilRelease;
  if (ignoreBackUntilRelease && !mappedInput.isPressed(MappedInputManager::Button::Back)) {
    ignoreBackUntilRelease = false;
  }

  if (!suppressBack &&
      ReaderUtils::handleBackNavigation(mappedInput, activityManager, txt ? txt->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<TxtReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  // Auto page turn: any Confirm/Back press cancels; otherwise advance on the timer.
  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }
    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      lastPageTurnTime = millis();
      if (currentPage < totalPages - 1) {
        currentPage++;
        requestUpdate();
      } else {
        automaticPageTurnActive = false;
        requestUpdate();
      }
      return;
    }
  }

  // Short Confirm release opens the reader menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openReaderMenu();
    return;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt, fromSide] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Long-press gestures mirror the EPUB reader and honor the same settings:
  //   - side Up/Down held + sideLongPressButtonBehavior == ORIENTATION_CHANGE -> rotate
  //   - front Left held    + longPressButtonBehavior     == BOOKMARK_AND_SYNC -> toggle bookmark
  // (TXT has no chapters or sync, so CHAPTER_SKIP and the hold-right sync are not handled.)
  const bool longPress = !fromTilt && mappedInput.getHeldTime() >= ReaderUtils::SKIP_HOLD_MS;
  const uint8_t lpBehavior = fromSide ? SETTINGS.sideLongPressButtonBehavior : SETTINGS.longPressButtonBehavior;

  if (longPress && lpBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (APP_STATE.activeOrientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (APP_STATE.activeOrientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (longPress && lpBehavior == SETTINGS.BOOKMARK_AND_SYNC) {
    // Hold left (page-back) = toggle bookmark; hold right has no TXT action.
    if (prevTriggered) {
      toggleBookmark();
    }
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.getReaderScreenMargin();
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed.
    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      int lineWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

      if (lineWidth <= viewportWidth) {
        outLines.push_back(line);
        lineBytePos = displayLen;  // Consumed entire display content
        line.clear();
        break;
      }

      // Find break point
      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextAdvanceX(cachedFontId, line.substr(0, breakPos).c_str(),
                                                      EpdFontFamily::REGULAR) > viewportWidth) {
        // Try to break at space
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          // Break at character boundary for UTF-8
          breakPos--;
          // Make sure we don't break in the middle of a UTF-8 sequence
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) {
            breakPos--;
          }
        }
      }

      if (breakPos == 0) {
        breakPos = 1;
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  // After an orientation-driven re-index, totalPages may have changed; restore
  // the reading position from the preserved fraction instead of the stale page.
  if (restorePendingFraction) {
    restorePendingFraction = false;
    if (totalPages > 0) {
      currentPage = static_cast<int>(pendingProgressFraction * (totalPages - 1) + 0.5f);
    }
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();

  // Capture screenshot of the rendered page if requested from the menu.
  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  // Scan the status bar too: a CJK title redirected to the SD fallback font joins this page's
  // single batch prewarm instead of triggering its own SD pass after the scope ends.
  if (txt) renderStatusBar();
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing == CrossPointSettings::TEXT_AA_ANTIALIASED) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title, 0, 0, true, isCurrentPageBookmarked());
}

void TxtReaderActivity::saveProgress() const {
  HalFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

void TxtReaderActivity::loadOrientation() {
  // Default to the global orientation; override with the per-book value if saved.
  APP_STATE.activeOrientation = SETTINGS.orientation;
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/orientation.bin", f)) {
    uint8_t data[2];
    if (f.read(data, 2) == 2 && data[0] == ORIENTATION_FILE_VERSION && data[1] < SETTINGS.ORIENTATION_COUNT) {
      APP_STATE.activeOrientation = data[1];
    }
  }
}

void TxtReaderActivity::saveOrientation() const {
  HalFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/orientation.bin", f)) {
    const uint8_t data[2] = {ORIENTATION_FILE_VERSION, APP_STATE.activeOrientation};
    f.write(data, 2);
  } else {
    LOG_ERR("TRS", "Failed to save per-book orientation");
  }
}

void TxtReaderActivity::applyOrientation(const uint8_t orientation) {
  if (APP_STATE.activeOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);

  // Preserve reading position as a fraction; the page index is rebuilt below and
  // totalPages may change with the new viewport width.
  pendingProgressFraction = totalPages > 1 ? static_cast<float>(currentPage) / (totalPages - 1) : 0.0f;
  restorePendingFraction = true;

  // Persist per-book so this book keeps the new orientation on next launch.
  APP_STATE.activeOrientation = orientation;
  saveOrientation();

  ReaderUtils::applyOrientation(renderer, APP_STATE.activeOrientation);

  // Force re-index in the new orientation. render() rebuilds via initializeReader().
  initialized = false;
  pageOffsets.clear();
  currentPageLines.clear();
}

void TxtReaderActivity::openReaderMenu() {
  const int progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  startActivityForResultNoThrow<TxtReaderMenuActivity>(
      [this](const ActivityResult& result) {
        // Always apply orientation / auto-page-turn changes even if cancelled.
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<TxtReaderMenuActivity::MenuAction>(menu.action));
        }
      },
      renderer, mappedInput, txt->getTitle(), currentPage + 1, totalPages, std::min(progressPercent, 100),
      APP_STATE.activeOrientation, selectedPageTurnOption);
}

void TxtReaderActivity::onReaderMenuConfirm(const TxtReaderMenuActivity::MenuAction action) {
  using MenuAction = TxtReaderMenuActivity::MenuAction;
  switch (action) {
    case MenuAction::READER_OPTIONS: {
      // Seed the preview with the current page's text so changes preview against the book.
      std::string sample;
      for (const auto& line : currentPageLines) {
        if (!sample.empty()) sample += ' ';
        sample += line;
      }
      auto options = makeUniqueNoThrow<ReaderOptionsActivity>(renderer, mappedInput, txt->getCachePath(),
                                                              SETTINGS.getReaderOverride(),
                                                              /*showMinSession=*/false, std::move(sample));
      if (!options) {
        LOG_ERR("TXT", "OOM: ReaderOptionsActivity");
        openReaderMenu();
        break;
      }
      startActivityForResult(std::move(options), [this](const ActivityResult&) {
        // Reload SD font at the (possibly new) size, then force a
        // re-index so the new font/margin/spacing takes effect.
        sdFontSystem.ensureLoaded(renderer);
        pendingProgressFraction = totalPages > 1 ? static_cast<float>(currentPage) / (totalPages - 1) : 0.0f;
        restorePendingFraction = true;
        initialized = false;
        pageOffsets.clear();
        currentPageLines.clear();
        ignoreBackUntilRelease = true;
        requestUpdate();
      });
      break;
    }
    case MenuAction::VIEW_BOOKMARKS: {
      openBookmarks();
      break;
    }
    case MenuAction::GO_TO_PERCENT: {
      const int initialPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
      startActivityForResultNoThrow<EpubReaderPercentSelectionActivity>(
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          },
          renderer, mappedInput, std::min(initialPercent, 100));
      break;
    }
    case MenuAction::DELETE_CACHE: {
      startActivityForResultNoThrow<ConfirmationActivity>(
          [this](const ActivityResult& confirmResult) {
            if (confirmResult.isCancelled) {
              return;
            }
            txt->clearCache();
            onGoHome();
          },
          renderer, mappedInput, tr(STR_CONFIRM_DELETE_CACHE), "");
      break;
    }
    case MenuAction::SCREENSHOT: {
      pendingScreenshot = true;
      requestUpdate();
      break;
    }
    case MenuAction::DISPLAY_QR: {
      std::string fullText;
      for (const auto& line : currentPageLines) {
        fullText += line;
        fullText += '\n';
      }
      if (!fullText.empty()) {
        startActivityForResultNoThrow<QrDisplayActivity>(
            [this](const ActivityResult&) { ignoreBackUntilRelease = true; }, renderer, mappedInput, fullText);
      } else {
        requestUpdate();
      }
      break;
    }
    case MenuAction::AUTO_PAGE_TURN:
      // Applied via the menu-exit callback (toggleAutoPageTurn); never returned
      // as a confirmed action.
      break;
  }
}

void TxtReaderActivity::jumpToPercent(const int percent) {
  const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  if (totalPages > 0) {
    currentPage = static_cast<int>(clamped / 100.0f * (totalPages - 1) + 0.5f);
    if (currentPage < 0) currentPage = 0;
    if (currentPage >= totalPages) currentPage = totalPages - 1;
  }
  requestUpdate();
}

void TxtReaderActivity::toggleAutoPageTurn(const uint8_t option) {
  selectedPageTurnOption = option;
  if (option == 0 || option >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[option];
  lastPageTurnTime = millis();
  automaticPageTurnActive = true;
}

void TxtReaderActivity::toggleBookmark() {
  // Snippet = first non-empty line on the current page.
  std::string snippet;
  for (const auto& line : currentPageLines) {
    if (!line.empty()) {
      snippet = line;
      break;
    }
  }
  TxtBookmarkStore::toggle(txt->getCachePath(), static_cast<uint32_t>(currentPage), snippet.c_str());
  reloadBookmarkPages();
  // Re-render so the status-bar bookmark indicator reflects the change.
  requestUpdate();
}

void TxtReaderActivity::openBookmarks() {
  startActivityForResultNoThrow<TxtReaderBookmarksActivity>(
      [this](const ActivityResult& result) {
        ignoreBackUntilRelease = true;
        // The viewer may have deleted entries; refresh the cached indicator set.
        reloadBookmarkPages();
        if (!result.isCancelled) {
          int page = static_cast<int>(std::get<PageResult>(result.data).page);
          if (page < 0) page = 0;
          if (page >= totalPages) page = totalPages - 1;
          currentPage = page;
        }
        requestUpdate();
      },
      renderer, mappedInput, txt->getCachePath(), totalPages);
}

void TxtReaderActivity::reloadBookmarkPages() {
  bookmarkPages.clear();
  if (!txt) {
    return;
  }
  const std::vector<TxtBookmark> stored = TxtBookmarkStore::load(txt->getCachePath());
  bookmarkPages.reserve(stored.size());
  for (const auto& bm : stored) {
    bookmarkPages.push_back(bm.page);
  }
}

bool TxtReaderActivity::isCurrentPageBookmarked() const {
  return std::find(bookmarkPages.begin(), bookmarkPages.end(), static_cast<uint32_t>(currentPage)) !=
         bookmarkPages.end();
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
