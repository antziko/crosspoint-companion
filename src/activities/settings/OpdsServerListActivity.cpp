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
#include "fontIds.h"
#include "util/OpdsFilename.h"

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
  // In settings mode, append two virtual items: "Add Server" and "Filename format".
  // (A-Z sort is a per-server toggle inside the server editor; the download folder is
  // derived per-server from the server name, so there is no global folder item.) In
  // picker mode, only real servers.
  if (!pickerMode) {
    count += 2;
  }
  return count;
}

void OpdsServerListActivity::onEnter() {
  Activity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  selectedIndex = 0;
  requestUpdate();
}

void OpdsServerListActivity::onExit() { Activity::onExit(); }

void OpdsServerListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (pickerMode) {
      activityManager.goHome(HomeMenuItem::OPDS_BROWSER);
    } else {
      finish();
    }
    return;
  }

  // Track whether Confirm was pressed inside this activity.
  // Releases from a press that originated in a parent activity (e.g. SettingsActivity fires on
  // wasPressed, leaving the release for us) are ignored so we don't auto-open the first row.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressActive = true;
  }

  // After a hold-duplicate fired, swallow input until Confirm is physically released so
  // the release doesn't also trigger a normal selection.
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
      confirmPressActive = false;
    }
    return;
  }

  const int serverCount = static_cast<int>(OPDS_STORE.getCount());

  // Hold Confirm on a real server row (settings mode only, room available) -> duplicate.
  if (confirmPressActive && !pickerMode && selectedIndex < serverCount &&
      OPDS_STORE.getCount() < OpdsServerStore::maxServers() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= DUPLICATE_HOLD_MS) {
    longPressFired = true;
    confirmPressActive = false;
    duplicateSelectedServer();
    return;
  }

  // Short tap: only act on releases whose press originated inside this activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmPressActive) {
      confirmPressActive = false;
      handleSelection();
    }
    return;
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (pickerMode) {
    // Picker mode: selecting a server navigates to the OPDS browser
    if (selectedIndex < serverCount) {
      const auto* server = OPDS_STORE.getServer(static_cast<size_t>(selectedIndex));
      if (server) {
        auto browser = makeUniqueNoThrow<OpdsBookBrowserActivity>(renderer, mappedInput, *server);
        if (!browser) {
          LOG_ERR("OPDSLIST", "OOM: OpdsBookBrowserActivity; staying on the server list");
          return;
        }
        activityManager.replaceActivity(std::move(browser));
      }
    }
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto resultHandler = [this](const ActivityResult&) {
    // Reload server list when returning from editor
    OPDS_STORE.loadFromFile();
    selectedIndex = 0;
  };

  if (selectedIndex < serverCount) {
    startActivityForResultNoThrow<OpdsSettingsActivity>(resultHandler, renderer, mappedInput, selectedIndex);
  } else if (selectedIndex == serverCount) {
    // "Add Server" virtual item
    startActivityForResultNoThrow<OpdsSettingsActivity>(resultHandler, renderer, mappedInput, -1);
  } else if (selectedIndex == serverCount + 1) {
    // "Filename format" virtual item: tap cycles through the available formats.
    SETTINGS.opdsFilenameFormat =
        static_cast<uint8_t>((SETTINGS.opdsFilenameFormat + 1) % static_cast<uint8_t>(OpdsFilenameFormat::Count));
    SETTINGS.saveToFile();
    requestUpdate();
  }
}

void OpdsServerListActivity::duplicateSelectedServer() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());
  if (selectedIndex >= serverCount) return;
  const auto* src = OPDS_STORE.getServer(static_cast<size_t>(selectedIndex));
  if (!src) return;

  const std::string body = src->name.empty() ? src->url : src->name;

  // Build the copy up-front; captured by value so the handler owns it.
  OpdsServer copy = *src;
  copy.name = (src->name.empty() ? src->url : src->name) + tr(STR_OPDS_COPY_SUFFIX);

  auto handler = [this, copy](const ActivityResult& res) {
    if (res.isCancelled) return;
    if (!OPDS_STORE.addServer(copy)) return;  // at-limit safety; already logged in store
    // Select the newly added copy.
    selectedIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
    requestUpdate(true);
  };

  startActivityForResultNoThrow<ConfirmationActivity>(std::move(handler), renderer, mappedInput,
                                                      tr(STR_OPDS_DUPLICATE_SERVER), body);
}

void OpdsServerListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OPDS_SERVERS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = getItemCount();

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_SERVERS));
  } else {
    const auto& servers = OPDS_STORE.getServers();
    const auto serverCount = static_cast<int>(servers.size());

    // Primary label: server name (falling back to URL if unnamed).
    // Secondary label: server URL (shown as subtitle when name is set).
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
        [&servers, serverCount](int index) -> std::string {
          if (index < serverCount) {
            const auto& server = servers[index];
            return server.name.empty() ? server.url : server.name;
          }
          if (index == serverCount) {
            return std::string(I18n::getInstance().get(StrId::STR_ADD_SERVER));
          }
          return std::string(I18n::getInstance().get(StrId::STR_OPDS_FILENAME_FORMAT));
        },
        [&servers, serverCount](int index) -> std::string {
          if (index < serverCount) {
            return servers[index].name.empty() ? std::string("") : servers[index].url;
          }
          if (index == serverCount + 1) {
            return std::string(I18n::getInstance().get(opdsFormatLabel(SETTINGS.opdsFilenameFormat)));
          }
          return std::string("");
        });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
