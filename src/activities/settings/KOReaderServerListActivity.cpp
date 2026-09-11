#include "KOReaderServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Hold threshold for long-press duplicate gesture (matches OPDS and recent-books patterns).
constexpr unsigned long DUPLICATE_HOLD_MS = 1000;
// UTF-8 encoding of U+25CF BLACK CIRCLE, used to mark the active server.
constexpr char ACTIVE_MARKER[] = "\xE2\x80\xA2 ";  // • U+2022 BULLET (in all Ubuntu UI fonts)
}  // namespace

int KOReaderServerListActivity::getItemCount() const {
  // Real servers + one virtual "Add Server" row
  return static_cast<int>(KOREADER_STORE.getCount()) + 1;
}

void KOReaderServerListActivity::onEnter() {
  Activity::onEnter();
  KOREADER_STORE.loadFromFile();
  selectedIndex = 0;
  requestUpdate();
}

void KOReaderServerListActivity::onExit() { Activity::onExit(); }

void KOReaderServerListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Track whether Confirm was pressed inside this activity.
  // Releases from a press that originated in a parent activity (e.g. SettingsActivity fires on
  // wasPressed, leaving the release for us) are ignored so we don't auto-open the first row.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressActive = true;
  }

  // After a hold-duplicate fired, swallow input until Confirm is physically released
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
      confirmPressActive = false;
    }
    return;
  }

  const int serverCount = static_cast<int>(KOREADER_STORE.getCount());

  // Hold Confirm on a real server row (room available) -> duplicate
  if (confirmPressActive && selectedIndex < serverCount &&
      KOREADER_STORE.getCount() < KOReaderCredentialStore::maxServers() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= DUPLICATE_HOLD_MS) {
    longPressFired = true;
    confirmPressActive = false;
    duplicateSelectedServer();
    return;
  }

  // Touch hold on a real server row = the Confirm hold above. The X4 Pro has no
  // Confirm pin at all (BoardConfig.h, XTEINK_X4_PRO), so without this the
  // duplicate gesture is unreachable there. Resolved before the tap:
  // wasScreenLongPress suppresses the rest of the contact, so the finger lift
  // cannot also open the row's editor.
  int holdX = 0;
  int holdY = 0;
  if (mappedInput.wasScreenLongPress(holdX, holdY)) {
    const int heldRow = listTouch_.indexAt(renderer, holdX, holdY);
    if (heldRow >= 0 && heldRow < serverCount && KOREADER_STORE.getCount() < KOReaderCredentialStore::maxServers()) {
      selectedIndex = heldRow;
      duplicateSelectedServer();
    }
    return;
  }

  // Short tap: only act on releases whose press originated inside this activity
  // A tap on a row selects and activates it in one go, like the FUI list screens.
  // A tap is never a hold, so it cannot reach the long-press branch above.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.indexAt(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0) selectedIndex = tappedRow;

  // confirmPressActive tracks a button press that began on this screen; a tap has no
  // such edge, so it activates directly.
  if (tappedRow >= 0) {
    handleSelection();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmPressActive) {
      confirmPressActive = false;
      handleSelection();
    }
    return;
  }

  // getItemCount() is always >= 1 (real servers + the virtual "Add Server" row),
  // so no empty-list guard is needed here.
  const int itemCount = getItemCount();
  buttonNavigator.onNext([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void KOReaderServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(KOREADER_STORE.getCount());

  auto resultHandler = [this](const ActivityResult&) {
    KOREADER_STORE.loadFromFile();
    selectedIndex = 0;
  };

  if (selectedIndex < serverCount) {
    startActivityForResultNoThrow<KOReaderSettingsActivity>(resultHandler, renderer, mappedInput, selectedIndex);
  } else {
    // "Add Server" virtual item
    startActivityForResultNoThrow<KOReaderSettingsActivity>(resultHandler, renderer, mappedInput, -1);
  }
}

void KOReaderServerListActivity::duplicateSelectedServer() {
  const auto serverCount = static_cast<int>(KOREADER_STORE.getCount());
  if (selectedIndex >= serverCount) return;
  const auto* src = KOREADER_STORE.getServer(static_cast<size_t>(selectedIndex));
  if (!src) return;

  // Build the copy up-front; captured by value so the handler owns it.
  KOReaderSyncServer copy = *src;
  copy.name = (src->name.empty() ? src->serverUrl : src->name) + tr(STR_OPDS_COPY_SUFFIX);

  const std::string body = src->name.empty() ? src->serverUrl : src->name;

  auto handler = [this, copy](const ActivityResult& res) {
    if (res.isCancelled) return;
    if (!KOREADER_STORE.addServer(copy)) return;
    // Select the newly added copy
    selectedIndex = static_cast<int>(KOREADER_STORE.getCount()) - 1;
    requestUpdate(true);
  };

  startActivityForResultNoThrow<ConfirmationActivity>(std::move(handler), renderer, mappedInput,
                                                      tr(STR_KOREADER_DUPLICATE_SERVER), body);
}

void KOReaderServerListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_SYNC_SERVERS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = getItemCount();
  const int activeIdx = KOREADER_STORE.getActiveIndex();

  const auto& servers = KOREADER_STORE.getServers();
  const auto serverCount = static_cast<int>(servers.size());

  listTouch_.record(Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex);
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
      [&servers, serverCount, activeIdx](int index) -> std::string {
        if (index < serverCount) {
          const auto& s = servers[index];
          std::string primary = s.name.empty() ? s.serverUrl : s.name;
          if (index == activeIdx) {
            primary = std::string(ACTIVE_MARKER) + primary;
          }
          return primary;
        }
        return std::string(I18n::getInstance().get(StrId::STR_ADD_SERVER));
      },
      [&servers, serverCount](int index) -> std::string {
        if (index < serverCount) {
          // Show URL as subtitle only when a name is set
          return servers[index].name.empty() ? std::string("") : servers[index].serverUrl;
        }
        return std::string("");
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
