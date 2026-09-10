#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "util/OpdsFilename.h"

namespace fui = freeink::ui;

namespace {
// Hold threshold for long-press duplicate gesture (matches RECENT_LONG_PRESS_MS on home screen).
constexpr unsigned long DUPLICATE_HOLD_MS = 1000;

// Label shown for the current OPDS filename format in the list subtitle.
StrId opdsFormatLabel(uint8_t format) {
  switch (format) {
    case static_cast<uint8_t>(OpdsFilenameFormat::TitleAuthor):
      return StrId::STR_FMT_TITLE_AUTHOR;
    case static_cast<uint8_t>(OpdsFilenameFormat::TitleOnly):
      return StrId::STR_FMT_TITLE;
    default:
      return StrId::STR_FMT_AUTHOR_TITLE;
  }
}
}  // namespace

int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // "Add Server" is offered in both modes: a user with no servers configured reached the
  // picker with nothing on it and no way out but Back (upstream #3318). Settings mode adds
  // "Filename format" on top. (A-Z sort is a per-server toggle inside the server editor; the
  // download folder is derived per-server from the server name, so there is no global folder
  // item.)
  count += pickerMode ? 1 : 2;
  return count;
}

OpdsServerListActivity::OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const bool pickerMode)
    : UiListActivity("OpdsServerList", renderer, mappedInput), pickerMode(pickerMode) {}

void OpdsServerListActivity::onEnter() {
  UiListActivity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  nav.selected = 0;
  rebuildRowItems();
}

// Rebuilds rowItems_ (labels/actionValue) and their backing label text from
// OPDS_STORE.
// Structural — call only when the server list actually reloads, not from
// buildScreen(). The format row's live subtitle is refreshed in place by
// buildScreen() every render instead, since it tracks a live SETTINGS value
// that can change without a server-list reload.
void OpdsServerListActivity::rebuildRowItems() {
  rowItems_.clear();
  serverLabels_.clear();
  const int itemCount = getItemCount();
  if (itemCount == 0) return;
  rowItems_.reserve(itemCount);

  const auto& servers = OPDS_STORE.getServers();
  const auto serverCount = static_cast<int>(servers.size());

  // Server rows read "name - url" on one label instead of a name/URL subtitle
  // pair, so a row costs one line when it fits. The joined text has no home in
  // the store, so it is owned here: at most maxServers() short strings, built
  // only on a list reload (not per repaint), and reserved up front so no
  // push_back can reallocate the pointers handed to rowItems_ below.
  serverLabels_.reserve(serverCount);
  for (int i = 0; i < serverCount; i++) {
    if (servers[i].name.empty()) {
      serverLabels_.push_back(servers[i].url);
    } else {
      serverLabels_.push_back(servers[i].name + " - " + servers[i].url);
    }
  }

  for (int i = 0; i < serverCount; i++) {
    fui::ListItem item;
    item.label = serverLabels_[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems_.push_back(item);
  }
  fui::ListItem addServer;
  addServer.label = tr(STR_ADD_SERVER);
  addServer.actionValue = static_cast<int16_t>(serverCount);
  rowItems_.push_back(addServer);

  if (!pickerMode) {
    // LOCAL(feat): no global "Download folder" row. Upstream has one backed by
    // SETTINGS.opdsDownloadFolder, which does not exist here — feat derives the
    // download folder per-server from the server name (see getItemCount), so the
    // format row sits at serverCount + 1, not upstream's serverCount + 2.
    fui::ListItem format;
    format.label = tr(STR_OPDS_FILENAME_FORMAT);
    format.actionValue = static_cast<int16_t>(serverCount + 1);
    rowItems_.push_back(format);  // subtitle refreshed per render below
  }
}

bool OpdsServerListActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void OpdsServerListActivity::onBackButton() {
  if (pickerMode) {
    activityManager.goHome(HomeMenuItem::OPDS_BROWSER);
  } else {
    finish();
  }
}

const char* OpdsServerListActivity::headerTitle() const { return tr(STR_OPDS_SERVERS); }

// LOCAL(feat): Confirm handling is overridden wholesale because feat has two
// behaviours upstream's base handler has no equivalent for.
//
// 1. Hold Confirm on a server row duplicates that server. This is a BUTTON
//    hold, so it cannot use the FUI onRowLongPress() hook — that fires only
//    for touch contacts on rows masked InputLongPress.
// 2. Confirm acts on RELEASE, and only when the matching PRESS happened inside
//    this activity. SettingsActivity opens this screen on wasPressed and leaves
//    the release behind; without confirmPressActive that stray release would
//    immediately activate row 0 on entry.
bool OpdsServerListActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressActive = true;
  }

  // After a hold-duplicate fired, swallow input until Confirm is physically
  // released so the release doesn't also trigger a normal selection.
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
      confirmPressActive = false;
    }
    return true;
  }

  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  // Hold Confirm on a real server row (settings mode only, room available) -> duplicate.
  if (confirmPressActive && !pickerMode && nav.selected < serverCount &&
      OPDS_STORE.getCount() < OpdsServerStore::maxServers() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= DUPLICATE_HOLD_MS) {
    longPressFired = true;
    confirmPressActive = false;
    duplicateSelectedServer();
    return true;
  }

  // Short tap: only act on releases whose press originated inside this activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmPressActive) {
      confirmPressActive = false;
      activateIndex(nav.selected);
    }
    return true;
  }

  return UiListActivity::handleButtons();
}

void OpdsServerListActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens an editor/browser or repaints a new value; a lingering
  // flash would gray an unrelated row.
  app.clearTapFlash();
  handleSelection();
  requestUpdate();
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (pickerMode) {
    // Picker mode: selecting a server navigates to the OPDS browser
    if (nav.selected < serverCount) {
      const auto* server = OPDS_STORE.getServer(static_cast<size_t>(nav.selected));
      if (server) {
        auto browser = makeUniqueNoThrow<OpdsBookBrowserActivity>(renderer, mappedInput, *server);
        if (!browser) {
          LOG_ERR("OPDSLIST", "OOM: OpdsBookBrowserActivity; staying on the server list");
          return;
        }
        activityManager.replaceActivity(std::move(browser));
      }
    } else {
      // The "Add Server" row, the picker's only row when no server is configured yet.
      auto editor = makeUniqueNoThrow<OpdsSettingsActivity>(renderer, mappedInput, -1);
      if (!editor) {
        LOG_ERR("OPDSLIST", "OOM: OpdsSettingsActivity");
        return;
      }
      startActivityForResult(std::move(editor), [this](const ActivityResult&) {
        OPDS_STORE.loadFromFile();
        rebuildRowItems();
        requestUpdate();
      });
    }
    return;
  }

  // Index layout: [servers 0..serverCount-1], [Add Server], [Filename format].
  // LOCAL(feat): no [Download folder] row between the last two — see rebuildRowItems().
  //
  // Taken from upstream: the format row now opens a picker showing all three
  // options, replacing feat's tap-to-cycle, which gave no way to see the choices
  // and required up to three round trips to reach the wanted one.
  if (nav.selected == serverCount + 1) {
    static constexpr StrId formatLabels[] = {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR,
                                             StrId::STR_FMT_TITLE};
    optionPopup.show(StrId::STR_OPDS_FILENAME_FORMAT, formatLabels, static_cast<int>(OpdsFilenameFormat::Count),
                     SETTINGS.opdsFilenameFormat, [this](int idx) {
                       SETTINGS.opdsFilenameFormat = static_cast<uint8_t>(idx);
                       SETTINGS.saveToFile();
                     });
    requestUpdate();
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto resultHandler = [this](const ActivityResult&) {
    // Reload server list when returning from editor. Under the render lock:
    // rebuildRowItems() clears rowItems_, whose label pointers the render task
    // dereferences (same race as FileBrowserActivity, #3034). Result handlers
    // are dispatched with the lock released, so taking it here is safe.
    RenderLock lock(*this);
    OPDS_STORE.loadFromFile();
    nav.selected = 0;
    rebuildRowItems();
  };

  // startActivityForResultNoThrow, not upstream's startActivityForResult with a
  // bare make_unique: make_unique aborts on OOM under -fno-exceptions.
  if (nav.selected < serverCount) {
    startActivityForResultNoThrow<OpdsSettingsActivity>(resultHandler, renderer, mappedInput, nav.selected);
  } else {
    // "Add Server" virtual item
    startActivityForResultNoThrow<OpdsSettingsActivity>(resultHandler, renderer, mappedInput, -1);
  }
}

// LOCAL(feat): no upstream equivalent — reached from the Confirm hold in
// handleButtons(), confirmed before it writes so a long press cannot silently
// clone a server.
void OpdsServerListActivity::duplicateSelectedServer() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());
  if (nav.selected >= serverCount) return;
  const auto* src = OPDS_STORE.getServer(static_cast<size_t>(nav.selected));
  if (!src) return;

  const std::string body = src->name.empty() ? src->url : src->name;

  // Build the copy up-front; captured by value so the handler owns it.
  OpdsServer copy = *src;
  copy.name = (src->name.empty() ? src->url : src->name) + tr(STR_OPDS_COPY_SUFFIX);

  auto handler = [this, copy](const ActivityResult& res) {
    if (res.isCancelled) return;
    if (!OPDS_STORE.addServer(copy)) return;  // at-limit safety; already logged in store
    {
      // The list grew, so the row array is stale; rebuild before selecting the
      // copy -- under the render lock, see the editor result handler above.
      RenderLock lock(*this);
      rebuildRowItems();
      nav.selected = static_cast<int>(OPDS_STORE.getCount()) - 1;
    }
    requestUpdate(true);
  };

  startActivityForResultNoThrow<ConfirmationActivity>(std::move(handler), renderer, mappedInput,
                                                      tr(STR_OPDS_DUPLICATE_SERVER), body);
}

void OpdsServerListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints; derived
  // from the safe area so board bezel insets apply (same as LanguageSelect).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int itemCount = getItemCount();
  if (itemCount == 0) {
    screen.centeredText(tr(STR_NO_SERVERS), screen.theme().smallText);
    return;
  }

  // rowItems_ (labels/actionValue) was built by rebuildRowItems() when the
  // server list last reloaded; only the format
  // row's live subtitle needs refreshing here (pointer reassignment onto an
  // already-owned string — no allocation).
  if (!pickerMode) {
    const auto serverCount = static_cast<int>(OPDS_STORE.getServers().size());
    // LOCAL(feat): only the format row's subtitle is live here. Upstream also
    // refreshes a [Download folder] row at serverCount + 1 from
    // SETTINGS.opdsDownloadFolder; feat has no such setting or row, so the
    // format row is at serverCount + 1 and indexing +2 would run off the end.
    rowItems_[serverCount + 1].subtitle = I18N.get(opdsFormatLabel(SETTINGS.opdsFilenameFormat));
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // The Settings list's label size on every board, on the plain row height, which
  // list() grows only for the rows that need it — a "name - url" label too long for
  // one line wraps to two, everything that fits stays at the dense single-line height.
  // maxLines = 2 also marks the style explicitly set — an all-default smallText fails
  // textStyleUnset and Screen::list() would substitute bodyText back.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props, /*hasSubtitle=*/false);
  screen.list(props);
}

void OpdsServerListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the base skeleton.
  UiListActivity::render(std::move(lock));
}
