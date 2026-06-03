#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/SdDebugLog.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr unsigned long GO_HOME_MS = 1000;  // hold BACK this long to jump to home

// On-SD filename for a book entry (no directory). Single source of truth so the
// downloader and the "already downloaded" indicator never diverge.
std::string bookFileName(const OpdsEntry& book) {
  // "Title - Author.epub" (or "Title.epub" when no author). This order is the
  // long-standing download convention — books already on the card use it, so the
  // downloader and the "already downloaded" marker must match it exactly.
  return StringUtils::sanitizeFilename(book.title + (book.author.empty() ? "" : " - " + book.author)) + ".epub";
}

// Download destination: the SD card root.
std::string bookFilePath(const OpdsEntry& book) { return "/" + bookFileName(book); }

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
    for (const std::string& existing : out) {
      if (existing == name) return;
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

// True if the book is already on the card: at the download root, or moved into
// the finished-books folder ("/read", see READ_FOLDER in EpubReaderActivity.cpp).
// Note: a finished book that collided on move may be "name (2).epub" in /read,
// which this base-name check won't catch — the common case is covered.
bool isBookOnDevice(const OpdsEntry& book) {
  for (const std::string& name : bookFileNameCandidates(book)) {
    if (Storage.exists(("/" + name).c_str()) || Storage.exists(("/read/" + name).c_str())) return true;
  }
  return false;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  // Fresh on-SD debug trace for this browsing session (readable at
  // /opds_debug.log without a serial monitor).
  SdDebugLog::setEnabled(true);
  SdDebugLog::clear();
  SdDebugLog::log("OPDS", "browser opened, free heap=%u", (unsigned)ESP.getFreeHeap());

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
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
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
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
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
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const auto& entry = entries[i];
      std::string displayText;
      if (entry.type == OpdsEntryType::NAVIGATION) {
        displayText = "> " + entry.title;
      } else {
        // Mark books already on the SD card (download root or finished "/read"
        // folder). Prefix (not suffix) so the marker survives truncatedText().
        // Re-checked each render, so a freshly downloaded book shows the mark
        // immediately on the next draw.
        const bool downloaded = isBookOnDevice(entry);
        displayText = (downloaded ? "* " : "") + entry.title;
        if (!entry.author.empty()) displayText += " - " + entry.author;
      }
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
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
  const auto dl = HttpDownloader::downloadToFile(url, kTmpFeed, nullptr, nullptr, server.username, server.password);
  if (dl != HttpDownloader::OK) {
    SdDebugLog::log("OPDS", "FETCH FAILED (http) code=%d, heap=%u", static_cast<int>(dl), (unsigned)ESP.getFreeHeap());
    Storage.remove(kTmpFeed);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

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
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
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
  // page only — not the whole catalog. Toggleable under System > OPDS Servers.
  if (SETTINGS.opdsSortAlphabetical) {
    std::sort(entries.begin(), entries.end(), [](const OpdsEntry& a, const OpdsEntry& b) {
      if (a.type != b.type) return a.type < b.type;  // NAVIGATION (0) before BOOK (1)
      return std::lexicographical_compare(
          a.title.begin(), a.title.end(), b.title.begin(), b.title.end(),
          [](unsigned char c1, unsigned char c2) { return std::tolower(c1) < std::tolower(c2); });
    });
  }

  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
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
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  std::string filename = bookFilePath(book);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      },
      nullptr, server.username, server.password);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
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
