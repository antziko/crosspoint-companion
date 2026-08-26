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

namespace fui = freeink::ui;

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

KOReaderSettingsActivity::KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   const int serverIndex)
    : UiListActivity("KOReaderSettings", renderer, mappedInput), serverIndex(serverIndex) {
  // Labels never change (unlike the values, which track editServer's fields
  // live), so they're set once here rather than every buildScreen() call.
  // LOCAL(feat): eleven rows, not upstream's eight. Listing fewer here would
  // leave the tail labels value-initialised to StrId(0) and render the wrong
  // strings on an existing server.
  static constexpr StrId ROW_LABELS[MAX_MENU_ITEMS] = {
      StrId::STR_SERVER_NAME,        // 0  Name
      StrId::STR_KOREADER_USERNAME,  // 1  Username
      StrId::STR_KOREADER_PASSWORD,  // 2  Password
      StrId::STR_SYNC_SERVER_URL,    // 3  Sync Server URL
      StrId::STR_DOCUMENT_MATCHING,  // 4  Document Matching
      StrId::STR_SEND_METADATA,      // 5  Send Metadata
      StrId::STR_SYNC_BEHAVIOR,      // 6  Sync Behavior
      StrId::STR_SET_AS_ACTIVE,      // 7  Set as Active
      StrId::STR_SIGN_UP,            // 8  Sign Up
      StrId::STR_AUTHENTICATE,       // 9  Authenticate
      StrId::STR_DELETE_SERVER,      // 10 Delete Server
  };
  for (int i = 0; i < MAX_MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(ROW_LABELS[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int KOReaderSettingsActivity::getMenuItemCount() const {
  if (isNewServer) return BASE_ITEMS_NEW;
  int count = BASE_ITEMS_EXISTING;
  if (KOREADER_STORE.getCount() > 1) count++;  // Delete only when not the last server
  return count;
}

const char* KOReaderSettingsActivity::headerTitle() const {
  return isNewServer ? tr(STR_ADD_SERVER) : tr(STR_KOREADER_SYNC);
}

void KOReaderSettingsActivity::onEnter() {
  UiListActivity::onEnter();

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
}

void KOReaderSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  app.clearTapFlash();
  handleSelection();
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
  if (nav.selected == ROW_NAME) {
    startActivityForResultNoThrow<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            editServer.name = std::get<KeyboardResult>(result.data).text;
            saveServer();
            requestUpdate();
          }
        },
        renderer, mappedInput, tr(STR_SERVER_NAME), editServer.name, 63, InputType::Text);

  } else if (nav.selected == ROW_USERNAME) {
    startActivityForResultNoThrow<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            editServer.username = std::get<KeyboardResult>(result.data).text;
            saveServer();
            requestUpdate();
          }
        },
        renderer, mappedInput, tr(STR_KOREADER_USERNAME), editServer.username, 64, InputType::Text);

  } else if (nav.selected == ROW_PASSWORD) {
    startActivityForResultNoThrow<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            editServer.password = std::get<KeyboardResult>(result.data).text;
            saveServer();
            requestUpdate();
          }
        },
        renderer, mappedInput, tr(STR_KOREADER_PASSWORD), editServer.password, 64, InputType::Text);

  } else if (nav.selected == ROW_URL) {
    const std::string prefillUrl = editServer.serverUrl.empty() ? "https://" : editServer.serverUrl;
    startActivityForResultNoThrow<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& text = std::get<KeyboardResult>(result.data).text;
            editServer.serverUrl = (text == "https://" || text == "http://") ? "" : text;
            saveServer();
            requestUpdate();
          }
        },
        renderer, mappedInput, tr(STR_SYNC_SERVER_URL), prefillUrl, 128, InputType::Url);

  } else if (nav.selected == ROW_DOC_MATCH) {
    // Toggle between Filename and Binary
    editServer.matchMethod = (editServer.matchMethod == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY
                                                                                       : DocumentMatchMethod::FILENAME;
    saveServer();
    requestUpdate();

  } else if (nav.selected == ROW_SEND_METADATA) {
    // Toggle whether document metadata is sent with progress sync for this server (#1820)
    editServer.sendMetadata = !editServer.sendMetadata;
    saveServer();
    requestUpdate();

  } else if (nav.selected == ROW_SYNC_BEHAVIOR) {
    // Toggle between Ask-every-time and Smart auto-resolve for this server (#2192)
    editServer.syncBehavior = (editServer.syncBehavior == KOReaderSyncBehavior::SMART)
                                  ? KOReaderSyncBehavior::ASK_EVERY_TIME
                                  : KOReaderSyncBehavior::SMART;
    saveServer();
    requestUpdate();

  } else if (nav.selected == ROW_SET_ACTIVE && !isNewServer) {
    if (serverIndex != KOREADER_STORE.getActiveIndex()) {
      KOREADER_STORE.setActiveIndex(serverIndex);
      KOREADER_STORE.saveToFile();
      requestUpdate();
    }

  } else if (nav.selected == ROW_SIGN_UP && !isNewServer) {
    // Sign Up: register a new account on this server with the entered credentials.
    if (editServer.username.empty() || editServer.password.empty()) {
      return;
    }
    // Pass serverIndex so the auth activity makes this server active (its creds + URL)
    // before registering; SIGN_UP mode calls createUser() instead of authenticate().
    startActivityForResultNoThrow<KOReaderAuthActivity>([](const ActivityResult&) {}, renderer, mappedInput,
                                                        serverIndex, KOReaderAuthActivity::Mode::SIGN_UP);

  } else if (nav.selected == ROW_AUTHENTICATE && !isNewServer) {
    // Credentials must be set before authenticating
    if (editServer.username.empty() || editServer.password.empty()) {
      return;
    }
    // Pass serverIndex so the auth activity makes this server active before testing,
    // and restores the previous active server if authentication fails.
    startActivityForResultNoThrow<KOReaderAuthActivity>([](const ActivityResult&) {}, renderer, mappedInput,
                                                        serverIndex);

  } else if (nav.selected == ROW_DELETE && !isNewServer) {
    // Delete only available when more than one server exists
    if (KOREADER_STORE.getCount() <= 1) return;
    // Confirm first so a mis-press on this row can't silently destroy a server.
    const int idx = serverIndex;
    const std::string& body = editServer.name.empty() ? editServer.serverUrl : editServer.name;
    startActivityForResultNoThrow<ConfirmationActivity>(
        [this, idx](const ActivityResult& res) {
          if (res.isCancelled) return;
          if (!KOREADER_STORE.removeServer(static_cast<size_t>(idx))) {
            LOG_ERR("KRS", "Failed to remove KOReader sync server at index %d", idx);
            showSaveError = true;
            requestUpdate();
            return;
          }
          finish();
        },
        renderer, mappedInput, tr(STR_DELETE_SERVER), body);
  }
}

void KOReaderSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_'s labels/actionValue were set once in the constructor; only the
  // live value text needs refreshing here, by assigning into the existing
  // rowValues_ strings (no array growth) rather than building a new
  // items/values vector on every render.
  const int menuItems = getMenuItemCount();
  const int activeIdx = KOREADER_STORE.getActiveIndex();
  for (int i = 0; i < menuItems; i++) {
    if (i == ROW_NAME) {
      rowValues_[i] = editServer.name.empty() ? tr(STR_NOT_SET) : editServer.name;
    } else if (i == ROW_USERNAME) {
      rowValues_[i] = editServer.username.empty() ? tr(STR_NOT_SET) : editServer.username;
    } else if (i == ROW_PASSWORD) {
      rowValues_[i] = editServer.password.empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == ROW_URL) {
      rowValues_[i] = editServer.serverUrl.empty() ? tr(STR_DEFAULT_VALUE) : editServer.serverUrl;
    } else if (i == ROW_DOC_MATCH) {
      rowValues_[i] = editServer.matchMethod == DocumentMatchMethod::FILENAME ? tr(STR_FILENAME) : tr(STR_BINARY);
    } else if (i == ROW_SEND_METADATA) {
      rowValues_[i] = editServer.sendMetadata ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == ROW_SYNC_BEHAVIOR) {
      rowValues_[i] =
          editServer.syncBehavior == KOReaderSyncBehavior::SMART ? tr(STR_SMART_SYNC) : tr(STR_ASK_EVERY_TIME);
    } else if (i == ROW_SET_ACTIVE) {
      // U+2022 bullet marks the server the reader currently syncs against.
      rowValues_[i] = (serverIndex == activeIdx) ? "\xE2\x80\xA2" : "";
    } else if (i == ROW_SIGN_UP || i == ROW_AUTHENTICATE) {
      rowValues_[i] = (editServer.username.empty() || editServer.password.empty())
                          ? std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]"
                          : "";
    } else {
      rowValues_[i].clear();
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(menuItems);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  // Row titles at the Settings screens' size (smallText) so every list in the
  // app reads at one size; labels that still don't fit wrap onto a second
  // line. maxLines=2 also marks the style explicitly set (an all-default
  // smallText fails textStyleUnset and Screen::list() would substitute
  // bodyText back, FONT_SLOT_SMALL being 0).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void KOReaderSettingsActivity::drawFooter() {
  UiListActivity::drawFooter();
  if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }
}
