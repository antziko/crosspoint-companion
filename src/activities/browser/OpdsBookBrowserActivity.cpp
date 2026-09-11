#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
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
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long GO_HOME_MS = 1000;  // hold BACK this long to jump to home
// Minimum contiguous heap required before bringing up an HTTPS connection. Below
// this the connect or an in-flight read can fail as an OOM-in-disguise and stall,
// so a guard remains — but it must be sized against the TLS stack actually in use.
//
// History: 44KB (default 16KB mbedTLS IN+OUT records) -> 24KB (shrunk records,
// DYNAMIC_BUFFER + IN_CONTENT_LEN=8192) -> 18KB. Every one of those steps was a
// false-rejection fix, and each was still priced against an mbedTLS record buffer.
//
// Lowered 18KB -> 10KB for wolfSSL. The 24KB and 18KB figures were both sized
// against mbedTLS record buffers ("~2.2x the ~8.2KB IN record"), and that record is
// gone twice over: the app is on wolfSSL (SecureHttpClient), and HAVE_MAX_FRAGMENT
// negotiates 2KB TLS records (platformio.ini:85-94, wolfSSL_UseMaxFragment in the
// SDK's SecureClient.cpp), so the receive buffer is ~2KB rather than ~17KB.
//
// The old gate was rejecting requests wolfSSL can serve. X3 log: two handshakes
// completed and ENDED holding largest8=11252 and 12276, while every later fetch and
// the book download were refused at largest=13812/14324 with ~41KB free — the
// browser went permanently dead after one big feed for want of a block nothing
// needed. 10KB keeps a fragmentation guard without pricing in a buffer that no
// longer exists; raise it if handshakes start failing with MEMORY_E.
constexpr size_t MIN_CONTIGUOUS_HEAP_FOR_TLS = 10 * 1024;

// Plain HTTP does no TLS handshake, so it needs no record buffers at all — only
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
  // Filename order is user-configurable (#2571). Default is Title-Author, this
  // branch's long-standing download convention. bookFileNameCandidates() below
  // matches every order, so the "already downloaded" marker stays correct
  // regardless of the chosen format.
  return opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
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

  const std::string title = book.title;
  const std::string author = book.author;

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
// Feed cache: the parsed-and-discarded feed body, kept on SD so returning to a page
// already visited re-parses it instead of re-downloading it.
//
// Worth doing because the re-fetch is not cheap on this device. Measured on X3
// (opds_debug.txt): the reload that follows every book download re-pulled a 130676-byte
// feed — a 1096ms TLS handshake plus a 2186ms body, ~3.3s and 130KB of radio to redisplay
// a list the user was looking at moments earlier. Backing out of a sub-catalog pays the
// same. The "already downloaded" marker is NOT read from the feed (refreshDownloadedCache
// stats the card), so a cached re-parse still shows a book that was just fetched.
//
// Keyed by navigation DEPTH, not by URL, and that is sound rather than lazy: every fetch
// writes the file for the depth it happens at, and the cache is only read by the two
// callers that return to a depth they just left (navigateBack, the post-download reload).
// A URL key would need a hash, a sidecar file to store it, and heap to compare it —
// against a stack that already tells us the answer. Forward navigation and the explicit
// retry never read it, so a feed the server has since changed is one Back away, not stuck.
constexpr int MAX_CACHE_DEPTH = 4;  // ~4 x <=200KB of SD; deeper levels simply refetch

// "" when the depth is past the cache, which the caller reads as "use the scratch file
// and delete it after the parse" — i.e. exactly the old behaviour.
std::string feedCachePath(const size_t depth) {
  if (depth >= static_cast<size_t>(MAX_CACHE_DEPTH)) return "";
  return "/.opds_c" + std::to_string(depth) + ".xml";
}

// Drop every cached feed. Runs on entry (a previous session's files are stale and their
// depths mean nothing here) and on exit (they are scratch, not user data).
void purgeFeedCache() {
  for (int d = 0; d < MAX_CACHE_DEPTH; d++) {
    const std::string path = feedCachePath(static_cast<size_t>(d));
    if (!path.empty()) Storage.remove(path.c_str());
  }
}

constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SEARCH = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr fui::ActionId ACTION_RETRY = 4;
// Book-download progress cadence. Percent-stepped so the repaint count is bounded
// at ~10 for any file size, with a hard floor between repaints. Both numbers are
// about SPI contention, not looks: see the callback in downloadBook().
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 10;
constexpr size_t DOWNLOAD_PROGRESS_STEP_BYTES = 128 * 1024;  // when the server sends no Content-Length
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_INTERVAL_MS = 10000;

// Feed-fetch progress cadence (see fetchFeed). Same reasoning as the download's
// below — on X3 a repaint takes the SPI bus the transfer writes over — but a feed is
// a shorter wait, so the floor between repaints is shorter.
constexpr int FEED_PROGRESS_STEP_PERCENT = 10;
constexpr size_t FEED_PROGRESS_STEP_BYTES = 32 * 1024;
constexpr unsigned long FEED_PROGRESS_MIN_INTERVAL_MS = 3000;

}  // namespace

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server)
    : Activity("OpdsBookBrowser", renderer, mappedInput),
      UiAppHost(renderer),
      buttonNavigator(),
      server(std::move(server)) {}

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
  // Any cached feed on the card belongs to a previous session, where the same depth
  // meant a different page. Depth is only a valid key within one browsing session.
  purgeFeedCache();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.on(ACTION_SEARCH, &OpdsBookBrowserActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &OpdsBookBrowserActivity::onCancelEvent, this);
  app.on(ACTION_RETRY, &OpdsBookBrowserActivity::onRetryEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
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
  purgeFeedCache();  // scratch, not user data

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::activateSelected() {
  if (entries.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[selectorIndex];
  entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->entries.size())) return;
  self->selectorIndex = event.value;
  // The tapped row leaves the screen either way (new feed or download view);
  // a lingering tap flash would gray an unrelated row on the next list.
  self->app.clearTapFlash();
  self->activateSelected();
}

void OpdsBookBrowserActivity::onSearchEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  self->launchSearch();
}

void OpdsBookBrowserActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DOWNLOADING) return;
  self->app.clearTapFlash();
  // cancelFetch is the flag handed to downloadToFile(); it is the only one it polls.
  self->cancelFetch = true;
}

void OpdsBookBrowserActivity::onRetryEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::ERROR) return;
  // The screen this tap flashed is replaced by the loading state either way.
  self->app.clearTapFlash();
  self->retryFromError();
}

void OpdsBookBrowserActivity::retryFromError() {
  // A retry with no link is not a retry: send the user to pick a network instead of
  // spending the fetch's timeout to rediscover that the radio is down.
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    launchWifiSelection();
    return;
  }
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate();
  fetchFeed(currentPath);
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
      retryFromError();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else {
      // The screen draws "Tap to retry" on a touch board, so a tap has to reach
      // ACTION_RETRY. Without this the X4 Pro dead-ends here: it has no Confirm
      // button, so the only offer the screen makes cannot be taken and one
      // transient failure ends the session. Back (a left-edge swipe) still exits.
      routeTouch(mappedInput);
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
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    // Touch goes through the FreeInkApp: render() registered every tap target
    // (rows, header search button); route the snapshot and let the registered
    // handlers dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed) {
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (route) return;  // dispatched to onRowEvent/onSearchEvent
      if (state != BrowserState::BROWSING) return;
    }

    if (!entries.empty()) {
      // Swipes scroll the viewport; the selection stays put (it may scroll
      // off-screen) and button navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? listNav.pageRows() : -listNav.pageRows();
        if (listNav.scrollBy(delta, static_cast<int>(entries.size()))) requestUpdate();
        return;
      }

      // Selection moves pull the viewport along (listNav.follow); the viewport
      // itself is otherwise only moved by the swipe above.
      const auto moveSelection = [this](const int index) {
        selectorIndex = index;
        listNav.selected = index;
        listNav.follow(static_cast<int>(entries.size()));
        requestUpdate();
      };
      const auto navigateNext = [this, moveSelection] {
        moveSelection(ButtonNavigator::nextIndex(selectorIndex, entries.size()));
      };
      const auto navigatePrevious = [this, moveSelection] {
        moveSelection(ButtonNavigator::previousIndex(selectorIndex, entries.size()));
      };

      // LOCAL(feat): list navigation is bound to the FRONT Left/Right buttons
      // only, not ButtonNavigator's default {Down,Right}/{Up,Left} sets that
      // upstream's onNextRelease/onPreviousRelease use. The side Up/Down
      // buttons are claimed below by resolveSideNavAction, whose hold gesture
      // cycles the display orientation — letting the default sets consume them
      // would silently kill that gesture.
      buttonNavigator.onRelease({MappedInputManager::Button::Right}, navigateNext);
      buttonNavigator.onRelease({MappedInputManager::Button::Left}, navigatePrevious);
      buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this, moveSelection] {
        moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), listNav.pageRows()));
      });
      buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this, moveSelection] {
        moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), listNav.pageRows()));
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

void OpdsBookBrowserActivity::rootScreen(UiScreen& screen, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

// Shared chrome for every state: reserve the firmware's button-hint band and
// draw the themed header (padding, centering, and rule come from the theme).
void OpdsBookBrowserActivity::screenHeader(UiScreen& screen, const bool withSearch) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  // Same top offset as every GUI.drawHeader caller, so the band lines up with
  // the rest of the firmware's screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().topPadding));
  fui::HeaderProps header;
  header.title = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  // The rule is top-bar chrome, so it goes with it; GUI.drawHeader's compact band drops its
  // underline the same way. The band itself stays — it carries the feed name.
  header.borderEdges = UITheme::isTopBarHidden() ? fui::EdgesNone : fui::EdgeBottom;
  if (withSearch && !searchTemplate.empty()) {
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    // Optically align the icon with the title glyphs: text hangs low in its
    // line cell by the font's internal leading; drop the button to match.
    const int titleFontId = uiScaleSpec().titleFontId;
    header.actionOffsetY =
        static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  }
  const auto& tokens = screen.theme();
  // Entry count for the feed on screen, right-aligned in the header band. It
  // shares the band with the search button (which only shrinks the text
  // content), so both fit. Must outlive screen.header() — header() draws
  // immediately, so this scope is enough.
  char countLabel[16];
  if (state == BrowserState::BROWSING && !entries.empty()) {
    snprintf(countLabel, sizeof(countLabel), "%u", static_cast<unsigned>(entries.size()));
    header.rightLabel = countLabel;
    // Draw the count in the title's font: header() bottom-aligns the label's
    // line box to the title's, so only a matching line height puts the two on
    // one baseline — the small font's shallower descender left the count
    // hanging below the server name and the search icon.
    header.subtitleText = tokens.titleText;
  }
  // Compact band. The theme's headerHeight also reserves the battery strip
  // that GUI.drawHeader draws on the other screens (Lyra: 84px for a 40px strip
  // plus the title line); this screen draws no battery, so the whole strip came
  // out as dead space between the server name and the first row. Take what the
  // title line needs instead, capped at the theme value so a theme whose band
  // is already tight is unaffected.
  const int16_t titleLineHeight = screen.target().lineHeight(tokens.titleText.font);
  int16_t bandHeight = static_cast<int16_t>(titleLineHeight + tokens.spaceMd * 2 + tokens.headerUnderline);
  if (bandHeight > tokens.headerHeight) bandHeight = tokens.headerHeight;
  screen.header(header, fui::LayoutAnchor::Top, bandHeight);
  // Same breathing room between header and content as the legacy screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  screenHeader(screen, true);

  if (entries.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().smallText);
    return;
  }

  // Transient per-render: sized once via reserve, points into `entries`
  // strings, freed on scope exit.
  // rowItems is built whenever entries changes (see rebuildRowItems(), called
  // from fetchFeed()/releaseEntries()) and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  // A book row's two lines are laid out on a ONE-line rowHeight below, which
  // spares no vertical padding of its own: without this the title and author
  // sit flush against the row edges and against the next row. Same breathing
  // room the theme builds into its own two-line row height.
  props.subtitleRowPadding = screen.theme().spaceMd;
  listNav.selected = selectorIndex;
  // Bold the SHORT line only, and never the title: bold glyphs are wider, so
  // the long line stays regular and fits more characters before it ellipsizes.
  // Which line the author is on follows the filename format (rebuildRowItems).
  const bool authorFirst = SETTINGS.opdsFilenameFormat == static_cast<uint8_t>(OpdsFilenameFormat::AuthorTitle);
  props.subtitleText = screen.theme().smallText;
  // Both styles are final from here: textStylesExplicit stops Screen::list()
  // from substituting the larger bodyText back over an all-default smallText,
  // which it cannot tell apart from "caller left this unset".
  props.textStylesExplicit = true;

  // Settings-list density on every board (see UiListActivity::resolveRowHeight; this
  // screen predates that base and syncs its own viewport directly): settings-sized labels
  // on the plain row height. Book rows carry a second line (see rebuildRowItems) and grow
  // to fit it, so navigation rows stay at the dense height.
  // The label stays single-line (the default maxLines) so a long title is ellipsized
  // rather than wrapped: a book row is then exactly two lines, never three.
  props.labelText = screen.theme().smallText;
  const int16_t rowHeight = static_cast<int16_t>(UITheme::getInstance().getMetrics().listRowHeight);
  props.rowHeight = rowHeight;
  if (authorFirst) {
    props.labelText.bold = true;
  } else {
    props.subtitleText.bold = true;
  }
  listNav.syncToProps(screen.body(), rowHeight, screen.theme().listRowGap, static_cast<int>(entries.size()), props);
  screen.list(props);
}

void OpdsBookBrowserActivity::buildDownloadScreen(UiScreen& screen) {
  screenHeader(screen, false);

  // Centered block: status line, book title, progress bar, cancel button.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.smallText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  // LOCAL(feat): the book title wraps over up to 2 lines rather than being
  // ellipsized on one (wrappedText falls back to truncatedText only if even 2
  // lines cannot hold it), so the block height depends on how it breaks.
  const auto titleLines =
      renderer.wrappedText(uiScaleSpec().bodyFontId, statusMessage.c_str(), screen.body().width - 40, 2);
  const int titleLineCount = titleLines.empty() ? 1 : static_cast<int>(titleLines.size());
  const int16_t blockH = static_cast<int16_t>(lh * (1 + titleLineCount) + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  if (titleLines.empty()) {
    screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);
  } else {
    for (size_t i = 0; i < titleLines.size(); i++) {
      screen.target().text(screen.takeTop(lh, i + 1 == titleLines.size() ? gap : 0), titleLines[i].c_str(), centered);
    }
  }

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  } else if (downloadProgress > 0) {
    // LOCAL(feat): the server sent no Content-Length (chunked / redirected
    // CDN), so there is no percentage to draw — show bytes received instead,
    // scaled to KB / MB / GB as the transfer grows. Upstream draws nothing at
    // all here, leaving a frozen, apparently-stalled screen.
    char sizeText[32];
    const double bytes = static_cast<double>(downloadProgress);
    if (bytes < 1024.0 * 1024.0) {
      snprintf(sizeText, sizeof(sizeText), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024.0 * 1024.0 * 1024.0) {
      snprintf(sizeText, sizeof(sizeText), "%.1f MB", bytes / (1024.0 * 1024.0));
    } else {
      snprintf(sizeText, sizeof(sizeText), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }
    screen.target().text(bar, sizeText, centered);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void OpdsBookBrowserActivity::buildStatusScreen(UiScreen& screen) {
  screenHeader(screen, false);

  fui::TextStyle centered = screen.theme().smallText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    // LOCAL(feat): errorMessage carries the real cause (e.g. "connect failed:
    // ESP_ERR_HTTP_CONNECT"), which does not fit one line on the narrower X3.
    // Wrap it over up to 3 lines instead of ellipsizing it away.
    const auto errLines =
        renderer.wrappedText(uiScaleSpec().bodyFontId, errorMessage.c_str(), screen.body().width - 40, 3);
    const int errLineCount = errLines.empty() ? 1 : static_cast<int>(errLines.size());
    const int16_t blockH =
        static_cast<int16_t>(lh * (1 + errLineCount + (showTapHint ? 1 : 0)) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    if (errLines.empty()) {
      screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    } else {
      for (size_t i = 0; i < errLines.size(); i++) {
        screen.target().text(screen.takeTop(lh, i + 1 == errLines.size() ? gap : 0), errLines[i].c_str(), centered);
      }
    }
    if (showTapHint) {
      screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
      // The whole body, not just the hint line: the hint says "tap to retry", and a
      // one-line target on an error screen the user is already squinting at is a worse
      // offer than the words promise. `body` is the pre-spacer rect, so it covers the
      // centred block wherever the wrapped message pushed it. Header excluded — its own
      // targets live there.
      screen.frame().hit(body, ACTION_RETRY, 0, fui::InputTouch);
    }
    return;
  }
  // CHECK_WIFI / LOADING (and the brief child-activity handoff states).
  screen.centeredText(statusMessage.c_str(), centered);
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::BROWSING: {
      // STR_GET, not STR_DOWNLOAD: the hint pill only fits ~72px at the 8pt hint face.
      const char* confirmLabel =
          (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_GET) : tr(STR_OPEN);
      const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  // Rows are variable height (a wrapped title grows its row, a subtitle row
  // grows past the dense row height), so the fixed-height viewport estimate can
  // fit fewer rows than it predicted and list() reports the correction back
  // through listNav. Rebuild until it settles, the same bounded loop as
  // UiListActivity::renderListFrame: each pass moves the viewport strictly
  // forward toward the selection, and a viewport starting at the selection
  // always draws it.
  for (int pass = 0; pass < 8; pass++) {
    renderer.clearScreen();
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderUi();
    if (!listNav.consumeRebuildNeeded()) break;
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path, const bool allowCache) {
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

  // Where this feed's body lives. A cacheable depth writes (and keeps) its own file;
  // deeper levels fall back to the shared scratch file and delete it after the parse.
  const std::string cachePath = feedCachePath(navigationHistory.size());
  const std::string feedPath = cachePath.empty() ? std::string(kTmpFeed) : cachePath;
  const bool keepAfterParse = !cachePath.empty();
  // A hit skips the radio entirely: no handshake, no transfer, no heap preflight.
  const bool fromCache = allowCache && keepAfterParse && Storage.exists(feedPath.c_str());

  // Free the current page's entries BEFORE the new feed's TLS connection comes
  // up. A full page (e.g. 50 bookmark entries) holds ~33KB; leaving it allocated
  // while mbedtls grabs its handshake record buffers fragments the heap (free
  // stays high but largest-contiguous collapses) and stalled the read mid-stream
  // on the X3 (see logs). This
  // mirrors downloadBook's swap-to-free. Nothing below reads the old entries:
  // they're fully replaced by the parser's results after the connection closes,
  // and the ERROR paths don't touch the list.
  //
  // Via releaseEntries(), not a bare swap of `entries`: the row buffers derived
  // from it (rowLabels/rowItems/downloadedCache) are just as much of the feed and
  // were being left behind, holding their blocks — and their pointers into the
  // freed entry strings — right through the handshake.
  //
  // Under the RENDER LOCK, with the two frees below it. All three hand memory the
  // render task reads back to the heap: releaseEntries() drops the row array
  // list() is walking plus the label/author strings it dereferences, and
  // releaseCache() drops the SD glyph arenas getTextWidth()/prewarm() walk. The
  // repaint queued when this screen resumed (returning from the search keyboard,
  // or from a Back that pops a subactivity) is still in flight here — that is a
  // use-after-free that panics with no message and a backtrace through
  // list() -> text() -> wrappedText(). The lock does NOT extend over the
  // transfer below, which must stay unlocked.
  {
    RenderLock lock;
    releaseEntries();

    // Hand the 32KB inflate window back to the heap for the feed parse. A large feed's
    // entry vector + strings OOMs (crashes) without it on the low-headroom X3. This
    // activity never inflates EPUB content, and onExit() silent-restarts (re-reserving
    // the window on a fresh heap), so it is never re-allocated under fragmentation.
    // Idempotent across the feed's repeated fetches.
    // NOTE: on X4 the freed mid-session block fragments rather than helps the HTTPS
    // handshake (30s reads / preflight "memory error"); enabled here per request, revert
    // to gpio.deviceIsX3() if HTTPS OPDS regresses on X4.
    InflateReader::releaseWindow();

    // Same reclaim for the SD font's resident glyph/kern arenas. The browsing list
    // renders CJK book titles through the SD fallback, so by the time a feed is
    // re-fetched those arenas are populated -- and they sit in exactly the 3-12KB
    // size class the TLS record buffer needs. Rebuilt on demand by the list repaint
    // after the transfer (one batch prewarm now, see GfxRenderer::
    // ensureSdGlyphsResident), so the cost is paid where there is heap for it.
    // releaseCache(), not clearCache(): the latter keeps the mini arena unless free
    // heap is already under its own 40KB floor, so it is not a guaranteed reclaim.
    // Placed BEFORE the preflight below so the measurement sees the recovered heap.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseCache();
    }
  }

  if (fromCache) {
    SdDebugLog::log("OPDS", "feed cache hit: depth=%u path=%s heap=%u", (unsigned)navigationHistory.size(),
                    feedPath.c_str(), (unsigned)ESP.getFreeHeap());
  } else {
    // Preflight the contiguous heap. If TLS can't get its buffers the connect or an
    // in-flight read fails as an OOM-in-disguise and can hang for minutes; fail fast
    // instead.
    //
    // This used to claim "entries were just freed, so a retry from the ERROR state has
    // more headroom and can succeed". That is FALSE and the device log disproves it four
    // times in a row: releaseEntries() runs above this check on the FIRST attempt too, so
    // a retry re-measures an identical heap. 2026-08-16 capture, four attempts across four
    // different URLs over 11 seconds, every one of them `largest=9204` to the byte, with
    // 36KB free. Nothing inside this activity defragments; the only thing that recovers it
    // is onExit()'s silent restart. Log the repeat explicitly so a capture shows the
    // difference between "tight heap" and "wedged heap" instead of four identical lines.
    const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBlock < minContiguousForUrl(url)) {
      SdDebugLog::log(
          "OPDS", "fetch aborted: low heap, largest=%u free=%u prevAbort=%u%s", (unsigned)largestBlock,
          (unsigned)ESP.getFreeHeap(), (unsigned)lastAbortLargestBlock,
          lastAbortLargestBlock > 0 && largestBlock <= lastAbortLargestBlock ? " WEDGED (retry cannot help)" : "");
      lastAbortLargestBlock = largestBlock;
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
    // Feed transfer progress. Feeds usually carry no Content-Length, so fall back to
    // bytes received (KB/MB) rather than a percentage when the server sends none.
    //
    // Throttling is deliberately coarse. This callback runs inside the socket read
    // loop, and every update queues a full-screen e-ink refresh on the single core the
    // transfer is also running on. The old rule repainted every 8KB, which put ~16
    // refreshes inside one 130KB feed that took 45s at 2888 B/s. Step by percent when
    // there is a Content-Length, by a coarse byte count when there is not, and never
    // twice inside FEED_PROGRESS_MIN_INTERVAL_MS however fast the bytes arrive. The
    // book-download callback below has had the same shape since #2957.
    size_t lastShown = 0;
    int lastRenderedPercent = -1;
    unsigned long lastProgressUpdateMs = 0;
    // Elapsed-time clock for the progress label.
    const uint32_t fetchStartMs = millis();
    cancelFetch = false;
    const auto dl = HttpDownloader::downloadToFile(
        url, feedPath.c_str(),
        [this, &lastShown, &lastRenderedPercent, &lastProgressUpdateMs, fetchStartMs](const size_t downloaded,
                                                                                      const size_t total) {
          // Poll Back every chunk (this fires per READ_CHUNK, not just per display
          // step) so the user can abort a slow feed instead of rebooting.
          // The downloader checks cancelFetch before the next socket read.
          mappedInput.update();
          if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
            cancelFetch = true;
            return;
          }
          const unsigned long now = millis();
          const bool complete = total > 0 && downloaded >= total;
          const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
          const bool stepped =
              total > 0 ? (lastRenderedPercent < 0 || percent >= lastRenderedPercent + FEED_PROGRESS_STEP_PERCENT)
                        : (downloaded - lastShown >= FEED_PROGRESS_STEP_BYTES);
          if (!complete && (!stepped || now - lastProgressUpdateMs < FEED_PROGRESS_MIN_INTERVAL_MS)) return;
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
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
      Storage.remove(feedPath.c_str());
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
      Storage.remove(feedPath.c_str());
      state = BrowserState::ERROR;
      // Append the real cause so a user without a serial cable sees it on screen.
      errorMessage = httpDetail.empty() ? std::string(tr(STR_FETCH_FEED_FAILED))
                                        : std::string(tr(STR_FETCH_FEED_FAILED)) + ": " + httpDetail;
      requestUpdate();
      return;
    }
  }  // end !fromCache

  // Parse runs after the connection closes (frees TLS heap). Label it: on a large
  // feed the parse itself is a noticeable, otherwise-silent stall.
  statusMessage = tr(STR_PARSING);
  requestUpdateAndWait();

  // The parser lives in its own scope so expat and its buffers are freed BEFORE the
  // sort / nav-link insert / rebuildRows below. That sequence is the tightest point in
  // the whole feed flow — the device log reaches it at free=10588 on a 40-entry feed,
  // and it is where "entries growth bailed" and "nav links dropped: low heap" fire.
  // Previously `parser` stayed alive through all of it for no reason: everything still
  // needed is copied out at the end of the scope. nextUrl/prevUrl are VALUES, not the
  // references they used to be — they pointed into the parser and would dangle here.
  std::string nextUrl;
  std::string prevUrl;
  {
    OpdsParser parser;
    auto rdbuf = makeUniqueNoThrow<uint8_t[]>(1024);
    HalFile feedFile;
    if (!rdbuf || !Storage.openFileForRead("OPDS", feedPath.c_str(), feedFile)) {
      SdDebugLog::log("OPDS", "FEED reopen failed / OOM, heap=%u", (unsigned)ESP.getFreeHeap());
      Storage.remove(feedPath.c_str());
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
    feedFile.close();  // before Storage.remove() on the same path
    // Keep a cacheable depth's file so Back / the post-download reload can re-parse it;
    // the scratch file is still deleted immediately, as before. purgeFeedCache() on exit
    // is what stops these accumulating.
    if (!keepAfterParse) Storage.remove(feedPath.c_str());

    if (!parser) {
      // A file that will not parse is worse than no cache — it would fail identically on
      // every Back. Drop it so the next visit refetches.
      Storage.remove(feedPath.c_str());
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
    SdDebugLog::log("OPDS", "fetch ok, %u entries%s, arena=%uB/%uchunks, heap=%u", (unsigned)parser.getEntries().size(),
                    parser.wasTruncated() ? " (TRUNCATED: feed too large for RAM)" : "", (unsigned)parser.arenaBytes(),
                    (unsigned)parser.arenaChunks(), (unsigned)ESP.getFreeHeap());

    searchTemplate = parser.getSearchTemplate();
    nextUrl = parser.getNextPageUrl();
    prevUrl = parser.getPrevPageUrl();
    // Takes the arena as well as the vector: entries are pointers into it (OpdsParser.h).
    entries = parser.takeEntries(entriesArena);
  }  // parser destroyed here: expat + its buffers return to the heap before the work below

  // Sort the page alphabetically (case-insensitive) by title, navigation folders
  // before books. Done before the prev/next links are added so those stay pinned at
  // the top/bottom. OPDS feeds are paginated server-side, so this orders the current
  // page only — not the whole catalog. Per-server toggle, set in the server editor.
  if (server.sortAlphabetical) {
    // Sorting moves 16-byte entries, never the text: the arena's chunks stay put, so every
    // title/author/href pointer survives the shuffle.
    std::sort(entries.begin(), entries.end(), [](const OpdsEntry& a, const OpdsEntry& b) {
      if (a.type != b.type) return a.type < b.type;  // NAVIGATION (0) before BOOK (1)
      const char* x = a.title;
      const char* y = b.title;
      for (; *x && *y; ++x, ++y) {
        const int cx = std::tolower(static_cast<unsigned char>(*x));
        const int cy = std::tolower(static_cast<unsigned char>(*y));
        if (cx != cy) return cx < cy;
      }
      return *x == '\0' && *y != '\0';  // shorter title sorts first on a common prefix
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
      // The hrefs are locals, so they must be copied into the arena: an entry holds a
      // pointer, and a pointer to prevUrl/nextUrl would dangle the moment this function
      // returns. tr() is a flash literal and needs no copy. A null add() means the arena
      // is out of chunks — drop that link rather than store a null href.
      const char* prevHref = prevUrl.empty() ? nullptr : entriesArena.add(prevUrl);
      const char* nextHref = nextUrl.empty() ? nullptr : entriesArena.add(nextUrl);
      entries.reserve(entries.size() + navLinks);
      if (prevHref != nullptr) {
        entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevHref});
      }
      if (nextHref != nullptr) {
        entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextHref});
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
  listNav.reset();
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  rebuildRowItems();
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
// Derives rowItems from entries. Called whenever entries changes
// (fetchFeed()/releaseEntries()) so buildBrowsingScreen() reuses the cached
// rows on every repaint instead of rebuilding them per render.
void OpdsBookBrowserActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(entries.size());
  // LOCAL(feat): rowLabels backs the "already on SD" marker. The prefix has to
  // live in an owned string because ListItem::label is a bare pointer and
  // entry.title has no room for it. Sized exactly, so no reallocation can
  // invalidate a c_str() taken during the loop.
  rowLabels.clear();
  rowLabels.reserve(entries.size());
  // A book row's two lines are ordered by the filename format, so a row reads
  // the way its download will be named on the card.
  const auto format = static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat);
  for (size_t i = 0; i < entries.size(); i++) {
    const auto& entry = entries[i];
    // Mark books already on the SD card (download folder or the finished
    // "read" subfolder). Prefix, not suffix, so the marker survives label
    // truncation. Read from the per-feed cache (refreshDownloadedCache) so a
    // repaint never re-stats the card.
    const bool downloaded = entry.type == OpdsEntryType::BOOK && i < downloadedCache.size() && downloadedCache[i];

    // Author-first only when there is an author to lead with and the format
    // asks for it; Title-only drops the second line entirely, and navigation
    // rows have no author at all, so both stay one line.
    const bool isBook = entry.type == OpdsEntryType::BOOK;
    const bool hasAuthor = isBook && entry.author[0] != '\0';
    const bool authorFirst = hasAuthor && format == OpdsFilenameFormat::AuthorTitle;
    const char* primary = authorFirst ? entry.author : entry.title;
    const char* secondary = nullptr;
    if (hasAuthor && format != OpdsFilenameFormat::TitleOnly) {
      secondary = authorFirst ? entry.title : entry.author;
    }

    // std::string(...) on both arms, not "* " + primary: primary is a const char*,
    // so the bare form would be pointer arithmetic that compiles and silently reads
    // past the literal.
    rowLabels.push_back(downloaded ? std::string("* ") + primary : std::string(primary));

    fui::ListItem item;
    item.label = rowLabels.back().c_str();
    // subtitle points straight into the arena — no copy, and valid as long as the feed is.
    item.subtitle = secondary;
    if (entry.type == OpdsEntryType::NAVIGATION) item.value = ">";
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

// Drop the feed and everything derived from it. Swap-with-empty on every vector,
// never clear(): clear() destroys the elements but keeps the capacity block, and
// the capacity is the point — a 40-row feed's rowLabels is 40 separate small
// allocations spread through the heap, which is what collapses largest8 even while
// free heap looks healthy (measured on X3: free=34008 with largest=9716, 524 bytes
// under the TLS preflight, refusing three retries in a row).
//
// This also un-dangles rowItems: item.subtitle points into entries[i].author, so
// freeing entries alone leaves the row buffer holding pointers into freed strings.
// closeRouting() stops the interaction table routing taps at the stale rows until
// the next render.
void OpdsBookBrowserActivity::releaseEntries() {
  closeRouting();
  std::vector<OpdsEntry>().swap(entries);
  // The arena goes with them: it holds every title/author/href, and it is the larger half
  // of a feed's footprint now that the entry vector is 16 bytes per row. Freeing the
  // entries without it would keep the text alive for nothing.
  entriesArena.clear();
  std::vector<uint8_t>().swap(downloadedCache);
  std::vector<freeink::ui::ListItem>().swap(rowItems);
  std::vector<std::string>().swap(rowLabels);
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
    // pop_back first, so navigationHistory.size() inside fetchFeed is the depth we are
    // returning TO — which is the depth whose cached body we want.
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath, /*allowCache=*/true);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  // Take OWNED copies of the text, not a copy of the entry. `book` references
  // entries[selectorIndex], and releaseEntries() below drops both the vector and the arena
  // its three fields point into to reclaim heap for the TLS handshake — so copying the
  // struct would only copy pointers and leave them dangling the moment the arena goes.
  // Three short strings is a cheap price for making that impossible rather than merely
  // ordered correctly.
  const std::string bookTitle = book.title;
  const std::string bookHref = book.href;
  const std::string bookAuthor = book.author;

  state = BrowserState::DOWNLOADING;
  statusMessage = bookTitle;
  downloadProgress = downloadTotal = 0;
  goHomeAfterCancel = false;
  // And-Wait, not requestUpdate(true). requestUpdate(true) only posts to the render task
  // (ActivityManager.cpp:349, xTaskNotify), so the repaint runs CONCURRENTLY with the code
  // below — including the contiguous-heap preflight, which then measures the heap at the
  // render's transient trough. Device log: "download aborted: low heap, largest=10228"
  // against a 10240 floor, twelve bytes short, while the very next TLS connection five
  // seconds later started from largest=24564. Waiting for the paint costs nothing here (the
  // TLS handshake that follows is far slower) and makes the measurement mean what it says.
  // This is also exactly what fetchFeed() does before its own handshake.
  requestUpdateAndWait();

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, bookHref.c_str());
  // Contain each server's books in a folder named after the server, so finished
  // books move into that folder's "read" subfolder (see EpubReaderActivity).
  const std::string folder = serverFolder(server.name);
  if (!folder.empty()) Storage.mkdir(folder.c_str());
  // Rebuilt from the owned copies, so the name survives releaseEntries() below.
  const OpdsEntry bookCopy{OpdsEntryType::BOOK, bookTitle.c_str(), bookAuthor.c_str(), bookHref.c_str()};
  std::string filename = bookFilePath(folder, bookCopy);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  // Free the feed list before connecting. A large feed (e.g. 50 bookmarks)
  // holds ~33KB and fragments the heap, so even with shrunk mbedtls buffers the
  // contiguous-heap preflight (MIN_CONTIGUOUS_HEAP_FOR_TLS) can abort or the
  // connect fails with ESP_ERR_HTTP_CONNECT (an OOM in disguise). Releasing
  // entries now gives TLS the contiguous headroom it needs; the list is
  // re-fetched after the download. Worst on the X3 (less RAM).
  const int savedIndex = selectorIndex;
  // Render lock for the same reason as fetchFeed's teardown. The And-Wait above
  // has already drained the in-flight repaint, so this is belt-and-braces — but
  // the invariant is "never free render-visible state unlocked", not "free it
  // after a paint that happens to have finished".
  {
    RenderLock lock;
    releaseEntries();  // the row buffers are part of the feed too — see releaseEntries()
    // And the SD font arenas the list render populated — see fetchFeed().
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseCache();
    }
  }
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  SdDebugLog::log("OPDS", "download start, heap=%u, largest=%u, url=%s", (unsigned)ESP.getFreeHeap(),
                  (unsigned)largestBlock, downloadUrl.c_str());

  // Same TLS heap preflight as fetchFeed: bail with a clear message rather than
  // stalling for minutes on an OOM-in-disguise connect/read. RETRY reloads the
  // feed (entries were freed above).
  if (largestBlock < minContiguousForUrl(downloadUrl)) {
    SdDebugLog::log(
        "OPDS", "download aborted: low heap, largest=%u prevAbort=%u%s", (unsigned)largestBlock,
        (unsigned)lastAbortLargestBlock,
        lastAbortLargestBlock > 0 && largestBlock <= lastAbortLargestBlock ? " WEDGED (retry cannot help)" : "");
    lastAbortLargestBlock = largestBlock;
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
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  cancelFetch = false;
  goHomeAfterCancel = false;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastShown, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        // Poll Back so a slow book download can be aborted instead of rebooting
        // (fires per chunk; downloader checks cancelFetch before the next read).
        mappedInput.update();
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelFetch = true;
          return;
        }
        downloadProgress = downloaded;
        downloadTotal = total;
        // This update() consumes the one-shot home event before the central
        // ActivityManager dispatch can see it, so honor it here: abort the
        // download, then exit to home once the abort unwinds.
        if (mappedInput.wasHomeGesture()) {
          cancelFetch = true;
          goHomeAfterCancel = true;
          return;
        }
        // Keep the on-screen Cancel button live: the activity loop is blocked
        // for the whole transfer, so the only chance to route a tap is here.
        routeTouch(mappedInput);
        // Throttle redraws hard. On X3 the SD card shares the display SPI bus
        // (BoardConfig.h XTEINK_X3: "Shares the display SPI bus (SCLK 8 / MOSI 10)"),
        // so a repaint does not merely compete for CPU — it holds the bus this
        // transfer needs for every write, stalling the socket read for the whole
        // refresh. Measured on X3: 1.3-2.2s per stall.
        //
        // The step is percent-only and the interval is a FLOOR, never a trigger.
        // #2957 brought upstream's rule, which repainted whenever
        // `now - lastProgressUpdateMs >= 5000` — a periodic trigger that fires on a
        // slow link no matter how little arrived, so a slower transfer bought itself
        // more refreshes and got slower still. The X3 log shows it plainly: stalls
        // spaced 4785/5420/4372ms apart, 25% of the transfer inside them, and both
        // downloads dying incomplete at ~200KB of 1.6MB.
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        const bool stepped = total > 0 ? (percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT)
                                       : (downloaded - lastShown >= DOWNLOAD_PROGRESS_STEP_BYTES);
        if (percent >= 100 || lastRenderedPercent < 0 ||
            (stepped && now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_INTERVAL_MS)) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          lastShown = downloaded;
          requestUpdate(true);
        }
      },
      &cancelFetch, server.username, server.password, &httpDetail);

  if (result == HttpDownloader::ABORTED) {
    // User cancelled mid-download. downloadToFile already removed the partial
    // file. Reload the feed (entries were freed above) so the list reappears.
    SdDebugLog::log("OPDS", "download cancelled by user, heap=%u", (unsigned)ESP.getFreeHeap());
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
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
    // Cached, for the same reason the OK path below is — and it matters MORE here. A
    // cancel usually follows a long transfer, which is exactly when contiguous heap is at
    // its worst, and this refetch was going to the network at that moment. Measured on X3
    // (opds_debug.txt): user cancelled a 7745578-byte download, this line then fetched a
    // 130676-byte feed that had completed fine minutes earlier, entered its handshake at
    // 12252 free / 6644 largest, and died at 80432 bytes with MEMORY_E — and because the
    // feed endpoints ignore Range, there was no resume. The user saw "Failed to fetch
    // feed" for a list the card already held byte-for-byte.
    fetchFeed(currentPath, /*allowCache=*/true);
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
    // Re-parse the cached body rather than re-pulling it: the download just freed the
    // heap, the list is byte-for-byte the one we left, and the new "downloaded" marker
    // comes from refreshDownloadedCache() stat-ing the card, not from the feed. Measured
    // saving on X3: a 1096ms handshake plus a 2186ms 130676-byte transfer.
    fetchFeed(currentPath, /*allowCache=*/true);
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
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  startActivityForResultNoThrow<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        // Swallow the release of the Confirm press that closed the keyboard only
        // when that button is actually still held on resume — a blanket flag set
        // at launch went stale on touch flows and ate the next genuine Confirm.
        consumeConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        // Same for Back: the keyboard cancels on the PRESS, so its release
        // lands here and used to fall through to navigateBack() — one Back
        // press closed the keyboard and also stepped up a feed level.
        consumeBack = mappedInput.isPressed(MappedInputManager::Button::Back);
        state = BrowserState::BROWSING;
        if (!result.isCancelled) {
          performSearch(std::get<KeyboardResult>(result.data).text);
        } else {
          requestUpdate();
        }
      },
      renderer, mappedInput, tr(STR_SEARCH));
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

  navigationHistory.push_back(currentPath);
  currentPath = url;

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
  // Render lock over the teardown: releaseEntries() below frees the row array and
  // the strings a queued repaint of the browsing list is still walking — see
  // fetchFeed's teardown for the crash that is.
  RenderLock wifiTeardownLock;

  // Hand the 32KB inflate window back BEFORE the radio comes up, not in fetchFeed()
  // (which runs after WiFi is already connected). Bringing esp_wifi + lwip up costs
  // ~53KB measured, so entering this screen with the window still held leaves the
  // scan nothing to work with: the X3 log shows "enter WifiSelection free=60296"
  // followed by "scan skipped: low heap free=6500 largest=2804" — a failure the user
  // sees as "Not enough memory" on every Show. The same release was added to
  // KOReaderSyncActivity::onEnter() and simply never reached this path.
  //
  // Safe here for the same reason it is safe in fetchFeed(): this activity never
  // inflates EPUB content and onExit() silent-restarts, which re-reserves the window
  // on a fresh heap. releaseWindow() is idempotent, so fetchFeed()'s call still
  // stands for the already-connected path that skips this screen.
  InflateReader::releaseWindow();

  // Drop the feed for the duration of the WiFi round trip. Re-entering this screen
  // from the ERROR state (the "WiFi connection failed" retry) used to run a scan on
  // top of a live 40-row feed, and cancelling out of it left that feed in place while
  // the radio stayed up — so the retry's fetch met a heap fragmented by both and was
  // refused with "Memory error", identically, on every attempt. Nothing needs the list
  // meanwhile: both branches of onWifiSelectionComplete() either re-fetch it or show
  // an error screen.
  releaseEntries();
  selectorIndex = 0;
  wifiTeardownLock.unlock();
  LOG_DBG("OPDS", "Released inflate window + feed before WiFi (heap: %u, largest: %u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResultNoThrow<WifiSelectionActivity>(
      [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); }, renderer, mappedInput);
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // "Not connected", not "connection failed". Since 0bf4d8d8 the picker reports the
    // radio's postcondition rather than which button closed it, so arriving here means
    // one thing — there is no usable connection — and it covers both a failed attempt
    // and a user who declined to make one. Calling a deliberate Cancel a failure named
    // a fault that did not occur.
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_NOT_CONNECTED);
    requestUpdate();
  }
}
