#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <OpdsStream.h>
#include <SdDebugLog.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
// List layout: first row baseline and row pitch (px). itemsPerPage() derives the
// visible row count from these and the current screen height so nothing draws
// past the bottom of the panel in any orientation.
constexpr int LIST_TOP_Y = 60;
constexpr int LIST_ROW_H = 30;
constexpr int LIST_BOTTOM_RESERVE = 40;     // button-hints strip (buttonHintsHeight)
constexpr unsigned long GO_HOME_MS = 1000;  // hold BACK this long to jump to home
// Minimum contiguous heap required before bringing up an HTTPS connection.
// Sized for the shrunk mbedtls record buffers (custom_sdkconfig: DYNAMIC_BUFFER
// + IN_CONTENT_LEN=8192/OUT=2048). With dynamic buffers the largest single
// contiguous allocation the handshake makes is the IN record (~8.2KB); 24KB
// gives ~3x headroom for that plus the HTTPClient RX/TX scratch. The old 44KB
// value was sized for the default 16KB IN+OUT record buffers and, post-shrink,
// false-rejected fetches that had ample heap: on-device logs showed aborts at
// largest=34804 (X3) and largest=45044 (X4, under the 45056 gate by 12 bytes)
// while total free was 68-86KB. Below this the connect or an in-flight read can
// still fail as an OOM-in-disguise and stall, so a guard remains — just smaller.
//
// Lowered 24KB -> 18KB: X4 SD traces showed the FIRST HTTPS fetch succeeds, then
// the handshake fragments the heap so the SECOND fetch sees largest=24564 and was
// false-rejected by the old 24576 gate (by 12 bytes) with 67KB total free. 18KB
// still gives ~2.2x the ~8.2KB IN record (the largest single handshake alloc), so
// a genuinely-too-fragmented heap is still caught.
constexpr size_t MIN_CONTIGUOUS_HEAP_FOR_TLS = 18 * 1024;

// Plain HTTP does no TLS handshake, so it needs no mbedTLS record buffers — only
// a few KB contiguous for rx/tx and the client struct. A local http:// OPDS
// server must not be rejected by the TLS-sized contiguous gate above.
constexpr size_t MIN_CONTIGUOUS_HEAP_FOR_HTTP = 8 * 1024;

// Contiguous-heap bar for a request, picked by URL scheme. `url` carries the
// scheme (https://, http://, or a relative path that resolves under server.url).
inline size_t minContiguousForUrl(const std::string& url) {
  return url.rfind("https://", 0) == 0 ? MIN_CONTIGUOUS_HEAP_FOR_TLS : MIN_CONTIGUOUS_HEAP_FOR_HTTP;
}

// On-SD filename for a book entry (no directory). Single source of truth so the
// downloader and the "already downloaded" indicator never diverge.
std::string bookFileName(const OpdsEntry& book) {
  // "Title - Author.epub" (or "Title.epub" when no author). This order is the
  // long-standing download convention — books already on the card use it, so the
  // downloader and the "already downloaded" marker must match it exactly.
  return StringUtils::sanitizeFilename(book.title + (book.author.empty() ? "" : " - " + book.author)) + ".epub";
}

// Per-server download folder, named after the OPDS server: "/<sanitized name>".
// Empty server name falls back to the card root ("") so unnamed servers keep the
// legacy root-download behavior.
std::string serverFolder(const std::string& serverName) {
  if (serverName.empty()) return "";
  return "/" + StringUtils::sanitizeFilename(serverName);
}

// Download destination for a book under the given server folder ("" = root).
std::string bookFilePath(const std::string& folder, const OpdsEntry& book) {
  return (folder.empty() ? "/" : folder + "/") + bookFileName(book);
}

// All plausible on-card filenames for a book entry, to make the "already
// downloaded" marker tolerant of naming-order differences. OPDS feeds (and the
// files already on the card) vary: author may sit in a separate field or be
// embedded in the title, and the "Author - Title" / "Title - Author" order is
// inconsistent across servers. Returns sanitized "*.epub" names, deduped.
std::vector<std::string> bookFileNameCandidates(const OpdsEntry& book) {
  std::vector<std::string> out;
  out.reserve(4);
  auto add = [&](const std::string& base) {
    if (base.empty()) return;
    std::string name = StringUtils::sanitizeFilename(base) + ".epub";
    if (std::any_of(out.begin(), out.end(), [&name](const std::string& existing) { return existing == name; })) {
      return;
    }
    out.push_back(std::move(name));
  };

  const std::string& title = book.title;
  const std::string& author = book.author;

  add(title);  // title-only / already-combined as the feed presents it
  if (!author.empty()) {
    add(title + " - " + author);  // canonical download order
    add(author + " - " + title);  // reversed
  } else {
    // No separate author: it may be embedded in the title as "A - B". Try the
    // swapped arrangement so a card file in the other order still matches.
    const size_t sep = title.rfind(" - ");
    if (sep != std::string::npos) {
      add(title.substr(sep + 3) + " - " + title.substr(0, sep));
    }
  }
  return out;
}

// True if the book is already on the card: in this server's download folder, its
// finished-books subfolder ("<folder>/read"), or the legacy card-root locations
// ("/" and "/read") from before per-server folders existed.
// Note: a finished book that collided on move may be "name (2).epub", which this
// base-name check won't catch — the common case is covered.
bool isBookOnDevice(const std::string& folder, const OpdsEntry& book) {
  const std::string base = folder.empty() ? "" : folder;  // "/readeck" or ""
  for (const std::string& name : bookFileNameCandidates(book)) {
    if (Storage.exists((base + "/" + name).c_str()) || Storage.exists((base + "/read/" + name).c_str())) return true;
    // Legacy root locations (books downloaded before per-server folders).
    if (Storage.exists(("/" + name).c_str()) || Storage.exists(("/read/" + name).c_str())) return true;
  }
  return false;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  // X3 HTTPS troubleshooting: enable the SD trace for the lifetime of this
  // activity (covers feed fetch + book download, both of which call runGet).
  // See SdDebugLog.h / SUMMARY.md Part B Appendix.
  SdDebugLog::setEnabled(true);

  // One of the few non-reader screens that follows SETTINGS.displayOrientation
  // (the hold-to-rotate gesture is handled in loop(), see resolveSideNavAction).
  ReaderUtils::applyOrientation(renderer, SETTINGS.displayOrientation);

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();

  SdDebugLog::setEnabled(false);

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  entries.clear();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  // Long-press BACK -> jump straight to home, instead of stepping back up
  // through every feed level. The lock swallows the eventual release so it
  // doesn't also trigger a short-press navigateBack().
  if (lockLongPressBack) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) lockLongPressBack = false;
    return;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    lockLongPressBack = true;
    onGoHome();
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    if (!entries.empty()) {
      const auto navigateNext = [this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      };
      const auto navigatePrevious = [this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      };

      // Front Left/Right: single-step on release + continuous page-jump while held.
      buttonNavigator.onRelease({MappedInputManager::Button::Right}, navigateNext);
      buttonNavigator.onRelease({MappedInputManager::Button::Left}, navigatePrevious);
      buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), itemsPerPage());
        requestUpdate();
      });
      buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), itemsPerPage());
        requestUpdate();
      });

      // Physical side Up/Down: single-step only (no continuous page-jump) --
      // holding them is reserved for the display-orientation-cycle gesture.
      switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Down)) {
        case ReaderUtils::SideNavAction::STEP:
          navigateNext();
          break;
        case ReaderUtils::SideNavAction::ROTATE:
          ReaderUtils::cycleDisplayOrientation(renderer, -1);
          requestUpdate();
          break;
        case ReaderUtils::SideNavAction::NONE:
          break;
      }
      switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Up)) {
        case ReaderUtils::SideNavAction::STEP:
          navigatePrevious();
          break;
        case ReaderUtils::SideNavAction::ROTATE:
          ReaderUtils::cycleDisplayOrientation(renderer, 1);
          requestUpdate();
          break;
        case ReaderUtils::SideNavAction::NONE:
          break;
      }
    }
  }
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    // Wrap the real-cause detail over up to 3 lines instead of single-line
    // ellipsis truncation, so the full error (e.g. "connect failed:
    // ESP_ERR_HTTP_CONNECT") is visible. wrappedText falls back to truncatedText
    // only if it can't fit even 3 lines (see GfxRenderer.cpp). X3 is narrower than X4.
    const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto errLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), pageWidth - 40, 3);
    int errY = pageHeight / 2 + 10;
    for (const auto& line : errLines) {
      renderer.drawCenteredText(UI_10_FONT_ID, errY, line.c_str());
      errY += lineHeight;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));

    // Show the full title wrapped over up to 2 lines instead of single-line
    // ellipsis truncation; wrappedText itself falls back to truncatedText only
    // if a title is so long it can't fit even 2 lines (see GfxRenderer.cpp).
    const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40, 2);
    int titleY = pageHeight / 2 - 10;
    for (const auto& line : titleLines) {
      renderer.drawCenteredText(UI_10_FONT_ID, titleY, line.c_str());
      titleY += lineHeight;
    }

    const int barY = titleY + 10;
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, barY, pageWidth - 100, 20}, downloadProgress, downloadTotal);
    } else if (downloadProgress > 0) {
      // Server sent no Content-Length (chunked / redirected CDN): no percentage,
      // so show bytes received so far, scaled to KB / MB / GB as it grows.
      char sizeText[32];
      const double bytes = static_cast<double>(downloadProgress);
      if (bytes < 1024.0 * 1024.0) {
        snprintf(sizeText, sizeof(sizeText), "%.1f KB", bytes / 1024.0);
      } else if (bytes < 1024.0 * 1024.0 * 1024.0) {
        snprintf(sizeText, sizeof(sizeText), "%.1f MB", bytes / (1024.0 * 1024.0));
      } else {
        snprintf(sizeText, sizeof(sizeText), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
      }
      renderer.drawCenteredText(UI_10_FONT_ID, barY, sizeText);
    }
    renderer.displayBuffer();
    return;
  }

  const char* confirmLabel =
      (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const int pageItems = itemsPerPage();
    const auto pageStartIndex = selectorIndex / pageItems * pageItems;
    renderer.fillRect(0, LIST_TOP_Y + (selectorIndex % pageItems) * LIST_ROW_H - 2, pageWidth - 1, LIST_ROW_H);

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + pageItems); i++) {
      const auto& entry = entries[i];
      std::string displayText;
      if (entry.type == OpdsEntryType::NAVIGATION) {
        displayText = "> " + entry.title;
      } else {
        // Mark books already on the SD card (download root or finished "/read"
        // folder). Prefix (not suffix) so the marker survives truncatedText().
        // Read from the per-feed cache (refreshDownloadedCache), so cursor moves
        // don't re-stat the SD card; a freshly downloaded book is re-cached by the
        // feed reload at the end of downloadBook().
        const bool downloaded = i < downloadedCache.size() && downloadedCache[i];
        displayText = (downloaded ? "* " : "") + entry.title;
        if (!entry.author.empty()) displayText += " - " + entry.author;
      }
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, LIST_TOP_Y + (i % pageItems) * LIST_ROW_H, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

int OpdsBookBrowserActivity::itemsPerPage() const {
  // Rows that fit between the list top and the button-hints strip in the current
  // orientation. getScreenHeight() is 800 in portrait, 480 in landscape, so this
  // is ~23 vs ~12 — preventing off-panel draws that flood the per-pixel LOG_ERR.
  const int avail = renderer.getScreenHeight() - LIST_TOP_Y - LIST_BOTTOM_RESERVE;
  return std::max(1, avail / LIST_ROW_H);
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  // Per-server extra query (e.g. "limit=20"). buildUrl strips query from relative
  // paths and Readeck hrefs carry no limit, so inject it on every feed fetch.
  // Guard against duplicating a param the server already echoed into a next/prev link.
  if (!server.extraQuery.empty() && url.find(server.extraQuery) == std::string::npos) {
    url += (url.find('?') == std::string::npos ? '?' : '&') + server.extraQuery;
  }
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  SdDebugLog::log("OPDS", "fetch start, heap=%u, url=%s", (unsigned)ESP.getFreeHeap(), url.c_str());
  // Two-phase fetch: download to a temp file first, then parse it AFTER the
  // HTTPS connection is closed. A live TLS connection holds ~70KB (mbedtls
  // record buffers + session) on top of the 48KB framebuffer, leaving only
  // ~5KB free during the transfer — enough to stream bytes to a file (like a
  // book download) but NOT enough for expat's working memory on a large feed,
  // which fails mid-read at ~5KB free. Closing the connection frees the 70KB
  // so the parse runs with full heap. (Streaming the parser concurrently with
  // the TLS read only worked for tiny feeds that fit in the 5KB sliver.)
  static constexpr const char* kTmpFeed = "/.opds_feed.tmp";

  // Free the current page's entries BEFORE the new feed's TLS connection comes
  // up. A full page (e.g. 50 bookmark entries) holds ~33KB; leaving it allocated
  // while mbedtls grabs its handshake record buffers fragments the heap (free
  // stays high but largest-contiguous collapses) and stalled the read mid-stream
  // on the X3 (see logs). This
  // mirrors downloadBook's swap-to-free. Nothing below reads the old entries:
  // they're fully replaced by the parser's results after the connection closes,
  // and the ERROR paths don't touch the list.
  std::vector<OpdsEntry>().swap(entries);

  // Hand the 32KB inflate window back to the heap for the feed parse. A large feed's
  // entry vector + strings OOMs (crashes) without it on the low-headroom X3. This
  // activity never inflates EPUB content, and onExit() silent-restarts (re-reserving
  // the window on a fresh heap), so it is never re-allocated under fragmentation.
  // Idempotent across the feed's repeated fetches.
  // NOTE: on X4 the freed mid-session block fragments rather than helps the HTTPS
  // handshake (30s reads / preflight "memory error"); enabled here per request, revert
  // to gpio.deviceIsX3() if HTTPS OPDS regresses on X4.
  InflateReader::releaseWindow();

  // Preflight the contiguous heap. If TLS can't get its buffers the connect or an
  // in-flight read fails as an OOM-in-disguise and can hang for minutes; fail fast
  // instead. entries were just freed, so a retry from the ERROR state has more
  // headroom and can succeed.
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largestBlock < minContiguousForUrl(url)) {
    SdDebugLog::log("OPDS", "fetch aborted: low heap, largest=%u free=%u", (unsigned)largestBlock,
                    (unsigned)ESP.getFreeHeap());
    LOG_ERR("OPDS", "Fetch aborted: low heap (largest=%u)", (unsigned)largestBlock);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

  // Show "Connecting..." before the blocking TLS handshake. The render task is
  // event-driven (no timer), so nothing repaints while we're stalled inside the
  // handshake — paint the label now (And-Wait) so the user sees the stage instead
  // of a frozen "Loading...".
  statusMessage = tr(STR_CONNECTING);
  requestUpdateAndWait();

  std::string httpDetail;
  // Feed transfer progress. Feeds usually carry no Content-Length, so show bytes
  // received (KB/MB) rather than a percentage. Throttle to every 8KB: each e-ink
  // repaint is slow and a small feed would otherwise flood the render task.
  size_t lastShown = 0;
  // Elapsed-time clock for the progress label.
  const uint32_t fetchStartMs = millis();
  cancelFetch = false;
  const auto dl = HttpDownloader::downloadToFile(
      url, kTmpFeed,
      [this, &lastShown, fetchStartMs](const size_t downloaded, const size_t total) {
        // Poll Back every chunk (this fires per READ_CHUNK, not just per 8KB
        // display step) so the user can abort a slow feed instead of rebooting.
        // The downloader checks cancelFetch before the next socket read.
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelFetch = true;
          return;
        }
        if (downloaded - lastShown < 8 * 1024 && !(total > 0 && downloaded >= total)) return;
        lastShown = downloaded;
        char sizeText[64];
        const unsigned elapsedS = (millis() - fetchStartMs) / 1000;
        const double bytes = static_cast<double>(downloaded);
        if (bytes < 1024.0 * 1024.0) {
          snprintf(sizeText, sizeof(sizeText), "%s %.0f KB (%us)", tr(STR_DOWNLOADING), bytes / 1024.0, elapsedS);
        } else {
          snprintf(sizeText, sizeof(sizeText), "%s %.1f MB (%us)", tr(STR_DOWNLOADING), bytes / (1024.0 * 1024.0),
                   elapsedS);
        }
        statusMessage = sizeText;
        requestUpdate(true);
      },
      &cancelFetch, server.username, server.password, &httpDetail);
  if (dl == HttpDownloader::ABORTED) {
    // User pressed Back during the transfer. Drop to ERROR (not a hard failure):
    // Confirm retries, Back steps up a level — both handled in loop(). Avoids
    // recursing into navigateBack() from inside fetchFeed on the main task stack.
    Storage.remove(kTmpFeed);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_LOADING_CANCELLED);
    consumeBack = true;  // swallow the Back release that triggered the cancel
    requestUpdate();
    return;
  }
  if (dl != HttpDownloader::OK) {
    SdDebugLog::log("OPDS", "FETCH FAILED (http) code=%d detail=%s heap=%u", static_cast<int>(dl),
                    httpDetail.empty() ? "?" : httpDetail.c_str(), (unsigned)ESP.getFreeHeap());
    LOG_ERR("OPDS", "Fetch failed: %s (url=%s)", httpDetail.empty() ? "?" : httpDetail.c_str(), url.c_str());
    Storage.remove(kTmpFeed);
    state = BrowserState::ERROR;
    // Append the real cause so a user without a serial cable sees it on screen.
    errorMessage = httpDetail.empty() ? std::string(tr(STR_FETCH_FEED_FAILED))
                                      : std::string(tr(STR_FETCH_FEED_FAILED)) + ": " + httpDetail;
    requestUpdate();
    return;
  }

  // Parse runs after the connection closes (frees TLS heap). Label it: on a large
  // feed the parse itself is a noticeable, otherwise-silent stall.
  statusMessage = tr(STR_PARSING);
  requestUpdateAndWait();

  OpdsParser parser;
  {
    auto rdbuf = makeUniqueNoThrow<uint8_t[]>(1024);
    HalFile feedFile;
    if (!rdbuf || !Storage.openFileForRead("OPDS", kTmpFeed, feedFile)) {
      SdDebugLog::log("OPDS", "FEED reopen failed / OOM, heap=%u", (unsigned)ESP.getFreeHeap());
      Storage.remove(kTmpFeed);
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
    for (int n = feedFile.read(rdbuf.get(), 1024); n > 0; n = feedFile.read(rdbuf.get(), 1024)) {
      parser.write(rdbuf.get(), static_cast<size_t>(n));
      if (parser.error()) break;
    }
    parser.flush();
  }
  Storage.remove(kTmpFeed);

  if (!parser) {
    SdDebugLog::log("OPDS", "PARSE FAILED: %s (line %ld), heap=%u", parser.getErrorDetail(), parser.getErrorLine(),
                    (unsigned)ESP.getFreeHeap());
    LOG_ERR("OPDS", "Parse failed: %s (line %ld)", parser.getErrorDetail(), parser.getErrorLine());
    state = BrowserState::ERROR;
    const char* parseDetail = parser.getErrorDetail();
    errorMessage = (parseDetail && parseDetail[0]) ? std::string(tr(STR_PARSE_FEED_FAILED)) + ": " + parseDetail
                                                   : std::string(tr(STR_PARSE_FEED_FAILED));
    requestUpdate();
    return;
  }
  SdDebugLog::log("OPDS", "fetch ok, %u entries%s, heap=%u", (unsigned)parser.getEntries().size(),
                  parser.wasTruncated() ? " (TRUNCATED: feed too large for RAM)" : "", (unsigned)ESP.getFreeHeap());

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entries = std::move(parser).getEntries();

  // Sort the page alphabetically (case-insensitive) by title, navigation folders
  // before books. Done before the prev/next links are added so those stay pinned at
  // the top/bottom. OPDS feeds are paginated server-side, so this orders the current
  // page only — not the whole catalog. Per-server toggle, set in the server editor.
  if (server.sortAlphabetical) {
    std::sort(entries.begin(), entries.end(), [](const OpdsEntry& a, const OpdsEntry& b) {
      if (a.type != b.type) return a.type < b.type;  // NAVIGATION (0) before BOOK (1)
      return std::lexicographical_compare(
          a.title.begin(), a.title.end(), b.title.begin(), b.title.end(),
          [](unsigned char c1, unsigned char c2) { return std::tolower(c1) < std::tolower(c2); });
    });
  }

  // Appending the prev/next nav links grows the just-moved, tightly-sized vector.
  // On the X3's starved heap (a 37KB feed leaves ~2KB largest-block) that bare-`new`
  // reallocation aborts() under -fno-exceptions. Reserve once, guarded by the largest
  // contiguous block; if it won't fit, drop the nav links rather than crash — the
  // page's books still render (the user just can't page-forward/back on that feed).
  const size_t navLinks = (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1);
  if (navLinks > 0) {
    const size_t needBytes = (entries.size() + navLinks) * sizeof(OpdsEntry) + 1024;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= needBytes) {
      entries.reserve(entries.size() + navLinks);
      if (!prevUrl.empty()) {
        entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
      }
      if (!nextUrl.empty()) {
        entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
      }
    } else {
      SdDebugLog::log("OPDS", "nav links dropped: low heap size=%u need=%u largest=%u", (unsigned)entries.size(),
                      (unsigned)needBytes, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
  }

  // Compute the on-SD marker once now, not per render. This is the single point
  // every feed (re)load funnels through, including the post-download reload at
  // the end of downloadBook().
  refreshDownloadedCache();

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

void OpdsBookBrowserActivity::refreshDownloadedCache() {
  downloadedCache.assign(entries.size(), 0);
  const std::string dlFolder = serverFolder(server.name);
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].type == OpdsEntryType::BOOK) {
      downloadedCache[i] = isBookOnDevice(dlFolder, entries[i]) ? 1 : 0;
    }
  }
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  // Copy by value: `book` references entries[selectorIndex], and we free the
  // entries vector below to reclaim heap for the TLS handshake.
  const OpdsEntry bookCopy = book;

  state = BrowserState::DOWNLOADING;
  statusMessage = bookCopy.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, bookCopy.href);
  // Contain each server's books in a folder named after the server, so finished
  // books move into that folder's "read" subfolder (see EpubReaderActivity).
  const std::string folder = serverFolder(server.name);
  if (!folder.empty()) Storage.mkdir(folder.c_str());
  std::string filename = bookFilePath(folder, bookCopy);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  // Free the feed list before connecting. A large feed (e.g. 50 bookmarks)
  // holds ~33KB and fragments the heap, so even with shrunk mbedtls buffers the
  // contiguous-heap preflight (MIN_CONTIGUOUS_HEAP_FOR_TLS) can abort or the
  // connect fails with ESP_ERR_HTTP_CONNECT (an OOM in disguise). Releasing
  // entries now gives TLS the contiguous headroom it needs; the list is
  // re-fetched after the download. Worst on the X3 (less RAM).
  const int savedIndex = selectorIndex;
  std::vector<OpdsEntry>().swap(entries);
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  SdDebugLog::log("OPDS", "download start, heap=%u, largest=%u, url=%s", (unsigned)ESP.getFreeHeap(),
                  (unsigned)largestBlock, downloadUrl.c_str());

  // Same TLS heap preflight as fetchFeed: bail with a clear message rather than
  // stalling for minutes on an OOM-in-disguise connect/read. RETRY reloads the
  // feed (entries were freed above).
  if (largestBlock < minContiguousForUrl(downloadUrl)) {
    SdDebugLog::log("OPDS", "download aborted: low heap, largest=%u", (unsigned)largestBlock);
    LOG_ERR("OPDS", "Download aborted: low heap (largest=%u)", (unsigned)largestBlock);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

  std::string httpDetail;
  // Throttle redraws: the callback fires every ~2KB, but each e-ink refresh is
  // slow. Only repaint every 64KB so the bar/byte-count advances without
  // flooding the render task.
  size_t lastShown = 0;
  cancelFetch = false;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastShown](const size_t downloaded, const size_t total) {
        // Poll Back so a slow book download can be aborted instead of rebooting
        // (fires per chunk; downloader checks cancelFetch before the next read).
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelFetch = true;
          return;
        }
        downloadProgress = downloaded;
        downloadTotal = total;
        if (downloaded - lastShown >= 64 * 1024 || (total > 0 && downloaded >= total)) {
          lastShown = downloaded;
          requestUpdate(true);
        }
      },
      &cancelFetch, server.username, server.password, &httpDetail);

  if (result == HttpDownloader::ABORTED) {
    // User cancelled mid-download. downloadToFile already removed the partial
    // file. Reload the feed (entries were freed above) so the list reappears.
    SdDebugLog::log("OPDS", "download cancelled by user, heap=%u", (unsigned)ESP.getFreeHeap());
    consumeBack = true;  // swallow the Back release that triggered the cancel
    // Drop back to LOADING before the reload: fetchFeed paints its own status
    // lines (Connecting.../Parsing.../byte counts) into `statusMessage`, and
    // those render as a single centered line in LOADING. Leaving `state` at
    // DOWNLOADING would additionally draw the fixed "Downloading..." label and
    // the stale downloadProgress/downloadTotal from the just-finished transfer
    // on top of it — duplicated, mismatched status lines (see screenshots).
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    downloadProgress = downloadTotal = 0;
    fetchFeed(currentPath);
    if (!entries.empty()) selectorIndex = std::min<int>(savedIndex, entries.size() - 1);
    requestUpdate();
    return;
  }

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    // Reload the feed so the list (and the new "downloaded" marker) reappears;
    // heap is free again. fetchFeed resets selectorIndex, so restore it after.
    // Same DOWNLOADING -> LOADING reset as the ABORTED path above, and for the
    // same reason: avoid stacking fetchFeed's status lines on the download UI.
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    downloadProgress = downloadTotal = 0;
    fetchFeed(currentPath);
    if (!entries.empty()) selectorIndex = std::min<int>(savedIndex, entries.size() - 1);
    requestUpdate();
    return;
  }

  SdDebugLog::log("OPDS", "DOWNLOAD FAILED code=%d detail=%s heap=%u", static_cast<int>(result),
                  httpDetail.empty() ? "?" : httpDetail.c_str(), (unsigned)ESP.getFreeHeap());
  LOG_ERR("OPDS", "Download failed: %s (url=%s)", httpDetail.empty() ? "?" : httpDetail.c_str(), downloadUrl.c_str());
  state = BrowserState::ERROR;
  // Show the real cause; RETRY reloads the feed (entries were freed above).
  errorMessage = httpDetail.empty() ? std::string(tr(STR_DOWNLOAD_FAILED))
                                    : std::string(tr(STR_DOWNLOAD_FAILED)) + ": " + httpDetail;
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
