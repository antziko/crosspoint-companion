#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Rows always present for an existing server.
// New servers only show the first BASE_ITEMS_NEW rows (no Set Active, Authenticate, or Delete).
constexpr int BASE_ITEMS_NEW =
    7;  // Name, Username, Password, Sync Server URL, Doc Matching, Send Metadata, Sync Behavior
constexpr int BASE_ITEMS_EXISTING = 10;  // + Set as Active + Sign Up + Authenticate

// Row indices (shared between getMenuItemCount, handleSelection, render)
constexpr int ROW_NAME = 0;
constexpr int ROW_USERNAME = 1;
constexpr int ROW_PASSWORD = 2;
constexpr int ROW_URL = 3;
constexpr int ROW_DOC_MATCH = 4;
constexpr int ROW_SEND_METADATA = 5;
constexpr int ROW_SYNC_BEHAVIOR = 6;
constexpr int ROW_SET_ACTIVE = 7;
constexpr int ROW_SIGN_UP = 8;
constexpr int ROW_AUTHENTICATE = 9;
constexpr int ROW_DELETE = 10;
}  // namespace

int KOReaderSettingsActivity::getMenuItemCount() const {
  if (isNewServer) return BASE_ITEMS_NEW;
  int count = BASE_ITEMS_EXISTING;
  if (KOREADER_STORE.getCount() > 1) count++;  // Delete only when not the last server
  return count;
}

void KOReaderSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  isNewServer = (serverIndex < 0);
  showSaveError = false;

  if (!isNewServer) {
    const auto* server = KOREADER_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was removed between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }

  requestUpdate();
}

void KOReaderSettingsActivity::onExit() { Activity::onExit(); }

void KOReaderSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int menuItems = getMenuItemCount();
  buttonNavigator.onNext([this, menuItems] {
    selectedIndex = (selectedIndex + 1) % static_cast<size_t>(menuItems);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, menuItems] {
    selectedIndex = (selectedIndex + static_cast<size_t>(menuItems) - 1) % static_cast<size_t>(menuItems);
    requestUpdate();
  });
}

bool KOReaderSettingsActivity::saveServer() {
  bool success = false;

  if (isNewServer) {
    success = KOREADER_STORE.addServer(editServer);
    if (success) {
      // Promote to existing so subsequent field edits update in-place
      isNewServer = false;
      serverIndex = static_cast<int>(KOREADER_STORE.getCount()) - 1;
    } else {
      LOG_ERR("KRS", "Failed to add KOReader sync server");
    }
  } else {
    success = KOREADER_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
    if (!success) {
      LOG_ERR("KRS", "Failed to update KOReader sync server at index %d", serverIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }
  return success;
}

void KOReaderSettingsActivity::handleSelection() {
  if (selectedIndex == ROW_NAME) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SERVER_NAME),
                                                                   editServer.name, 63, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               editServer.name = std::get<KeyboardResult>(result.data).text;
                               saveServer();
                               requestUpdate();
                             }
                           });

  } else if (selectedIndex == ROW_USERNAME) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_USERNAME),
                                                                   editServer.username, 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               editServer.username = std::get<KeyboardResult>(result.data).text;
                               saveServer();
                               requestUpdate();
                             }
                           });

  } else if (selectedIndex == ROW_PASSWORD) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOREADER_PASSWORD),
                                                                   editServer.password, 64, InputType::Password),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               editServer.password = std::get<KeyboardResult>(result.data).text;
                               saveServer();
                               requestUpdate();
                             }
                           });

  } else if (selectedIndex == ROW_URL) {
    const std::string prefillUrl = editServer.serverUrl.empty() ? "https://" : editServer.serverUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SYNC_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& text = std::get<KeyboardResult>(result.data).text;
                               editServer.serverUrl = (text == "https://" || text == "http://") ? "" : text;
                               saveServer();
                               requestUpdate();
                             }
                           });

  } else if (selectedIndex == ROW_DOC_MATCH) {
    // Toggle between Filename and Binary
    editServer.matchMethod = (editServer.matchMethod == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY
                                                                                       : DocumentMatchMethod::FILENAME;
    saveServer();
    requestUpdate();

  } else if (selectedIndex == ROW_SEND_METADATA) {
    // Toggle whether document metadata is sent with progress sync for this server (#1820)
    editServer.sendMetadata = !editServer.sendMetadata;
    saveServer();
    requestUpdate();

  } else if (selectedIndex == ROW_SYNC_BEHAVIOR) {
    // Toggle between Ask-every-time and Smart auto-resolve for this server (#2192)
    editServer.syncBehavior = (editServer.syncBehavior == KOReaderSyncBehavior::SMART)
                                  ? KOReaderSyncBehavior::ASK_EVERY_TIME
                                  : KOReaderSyncBehavior::SMART;
    saveServer();
    requestUpdate();

  } else if (selectedIndex == ROW_SET_ACTIVE && !isNewServer) {
    if (serverIndex != KOREADER_STORE.getActiveIndex()) {
      KOREADER_STORE.setActiveIndex(serverIndex);
      KOREADER_STORE.saveToFile();
      requestUpdate();
    }

  } else if (selectedIndex == ROW_SIGN_UP && !isNewServer) {
    // Sign Up: register a new account on this server with the entered credentials.
    if (editServer.username.empty() || editServer.password.empty()) {
      return;
    }
    // Pass serverIndex so the auth activity makes this server active (its creds + URL)
    // before registering; SIGN_UP mode calls createUser() instead of authenticate().
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, serverIndex, KOReaderAuthActivity::Mode::SIGN_UP),
        [](const ActivityResult&) {});

  } else if (selectedIndex == ROW_AUTHENTICATE && !isNewServer) {
    // Credentials must be set before authenticating
    if (editServer.username.empty() || editServer.password.empty()) {
      return;
    }
    // Pass serverIndex so the auth activity makes this server active before testing,
    // and restores the previous active server if authentication fails.
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, serverIndex),
                           [](const ActivityResult&) {});

  } else if (selectedIndex == ROW_DELETE && !isNewServer) {
    // Delete only available when more than one server exists
    if (KOREADER_STORE.getCount() <= 1) return;
    // Confirm first so a mis-press on this row can't silently destroy a server.
    const int idx = serverIndex;
    const std::string& body = editServer.name.empty() ? editServer.serverUrl : editServer.name;
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_SERVER), body),
                           [this, idx](const ActivityResult& res) {
                             if (res.isCancelled) return;
                             if (!KOREADER_STORE.removeServer(static_cast<size_t>(idx))) {
                               LOG_ERR("KRS", "Failed to remove KOReader sync server at index %d", idx);
                               showSaveError = true;
                               requestUpdate();
                               return;
                             }
                             finish();
                           });
  }
}

void KOReaderSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const char* header = isNewServer ? tr(STR_ADD_SERVER) : tr(STR_KOREADER_SYNC);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int menuItems = getMenuItemCount();
  const int activeIdx = KOREADER_STORE.getActiveIndex();

  static constexpr StrId ROW_LABELS[] = {
      StrId::STR_SERVER_NAME,        // 0 Name
      StrId::STR_KOREADER_USERNAME,  // 1 Username
      StrId::STR_KOREADER_PASSWORD,  // 2 Password
      StrId::STR_SYNC_SERVER_URL,    // 3 Sync Server URL
      StrId::STR_DOCUMENT_MATCHING,  // 4 Document Matching
      StrId::STR_SEND_METADATA,      // 5 Send Metadata
      StrId::STR_SYNC_BEHAVIOR,      // 6 Sync Behavior
      StrId::STR_SET_AS_ACTIVE,      // 7 Set as Active
      StrId::STR_SIGN_UP,            // 8 Sign Up
      StrId::STR_AUTHENTICATE,       // 9 Authenticate
  };

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, menuItems, static_cast<int>(selectedIndex),
      [this](int index) -> std::string {
        if (index < BASE_ITEMS_EXISTING) {
          return std::string(I18N.get(ROW_LABELS[index]));
        }
        return std::string(tr(STR_DELETE_SERVER));
      },
      nullptr, nullptr,
      [this, activeIdx](int index) -> std::string {
        if (index == ROW_NAME) {
          return editServer.name.empty() ? std::string(tr(STR_NOT_SET)) : editServer.name;
        } else if (index == ROW_USERNAME) {
          return editServer.username.empty() ? std::string(tr(STR_NOT_SET)) : editServer.username;
        } else if (index == ROW_PASSWORD) {
          return editServer.password.empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == ROW_URL) {
          return editServer.serverUrl.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : editServer.serverUrl;
        } else if (index == ROW_DOC_MATCH) {
          return editServer.matchMethod == DocumentMatchMethod::FILENAME ? std::string(tr(STR_FILENAME))
                                                                         : std::string(tr(STR_BINARY));
        } else if (index == ROW_SEND_METADATA) {
          return editServer.sendMetadata ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
        } else if (index == ROW_SYNC_BEHAVIOR) {
          return editServer.syncBehavior == KOReaderSyncBehavior::SMART ? std::string(tr(STR_SMART_SYNC))
                                                                        : std::string(tr(STR_ASK_EVERY_TIME));
        } else if (index == ROW_SET_ACTIVE) {
          return (serverIndex == activeIdx) ? std::string("\xE2\x80\xA2") : std::string("");
        } else if (index == ROW_SIGN_UP || index == ROW_AUTHENTICATE) {
          if (editServer.username.empty() || editServer.password.empty()) {
            return std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
          }
          return std::string("");
        }
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }

  renderer.displayBuffer();
}
