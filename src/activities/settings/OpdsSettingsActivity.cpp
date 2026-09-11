#include "OpdsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Editable fields: Name, URL, Username, Password, Sort A-Z, Extra query.
// Existing servers also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 6;
constexpr int PASSWORD_ROW = 3;
// Same hold the sibling server list uses for its duplicate gesture, so the two screens
// answer a hold at the same moment.
constexpr unsigned long REVEAL_HOLD_MS = 1000;
}  // namespace

OpdsSettingsActivity::OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const int serverIndex)
    : UiListActivity("OpdsSettings", renderer, mappedInput, /*wantsTouchLongPress=*/true), serverIndex(serverIndex) {
  // Labels never change (unlike the values, which track editServer's fields
  // live), so they're set once here rather than every buildScreen() call.
  // LOCAL(feat): six fields, not upstream's four — feat adds the Sort A-Z toggle
  // and the extra-query field. Listing only four here would leave the last two
  // labels value-initialised to StrId(0) and render the wrong strings.
  static constexpr StrId fieldNames[BASE_ITEMS] = {
      StrId::STR_SERVER_NAME, StrId::STR_OPDS_SERVER_URL,        StrId::STR_USERNAME,
      StrId::STR_PASSWORD,    StrId::STR_OPDS_SORT_ALPHABETICAL, StrId::STR_OPDS_EXTRA_QUERY};
  for (int i = 0; i < BASE_ITEMS; i++) {
    fieldRowItems[i].label = I18N.get(fieldNames[i]);
    fieldRowItems[i].actionValue = static_cast<int16_t>(i);
  }
  fieldRowItems[BASE_ITEMS].label = tr(STR_DELETE_SERVER);
  fieldRowItems[BASE_ITEMS].actionValue = static_cast<int16_t>(BASE_ITEMS);
}

int OpdsSettingsActivity::getMenuItemCount() const {
  return isNewServer ? BASE_ITEMS : BASE_ITEMS + 1;  // +1 for Delete
}

void OpdsSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  isNewServer = (serverIndex < 0);
  showSaveError = false;

  if (!isNewServer) {
    // Edit flow: copy the selected server into local editable state.
    // Changes are persisted field-by-field through saveServer().
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was deleted between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }
}

void OpdsSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens a keyboard or leaves the screen; a lingering flash would
  // gray an unrelated row.
  app.clearTapFlash();
  revealPassword = false;
  handleSelection();
}

// Show the stored password in place of "******". The editor still opens EMPTY (see the
// Password branch in handleSelection): revealing is display-only, so it carries none of the
// hazard prefilling does -- there is nothing to type onto and nothing to save back.
void OpdsSettingsActivity::togglePasswordReveal() {
  revealPassword = !revealPassword;
  requestUpdate();
}

// Touch counterpart of the Confirm hold below. The X4 Pro has no Confirm pin at all
// (BoardConfig.h, XTEINK_X4_PRO), so without this the reveal is unreachable there.
void OpdsSettingsActivity::onRowLongPress(const int index) {
  if (index != PASSWORD_ROW || editServer.password.empty()) return;
  nav.selected = index;
  app.clearTapFlash();
  togglePasswordReveal();
}

// Confirm is overridden wholesale for the same reason the server list overrides it: a
// BUTTON hold cannot come through onRowLongPress(), which fires only for touch contacts on
// rows masked InputLongPress. The press-origin latch is kept here too, so a release left
// behind by the screen that opened this one cannot activate row 0 on entry.
bool OpdsSettingsActivity::handleButtons() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressActive = true;

  // After a reveal fires, swallow input until Confirm is physically released, or that
  // release would also open the password keyboard.
  if (revealHoldFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      revealHoldFired = false;
      confirmPressActive = false;
    }
    return true;
  }

  if (confirmPressActive && nav.selected == PASSWORD_ROW && !editServer.password.empty() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= REVEAL_HOLD_MS) {
    revealHoldFired = true;
    confirmPressActive = false;
    togglePasswordReveal();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmPressActive) {
      confirmPressActive = false;
      activateIndex(nav.selected);
    }
    return true;
  }

  return UiListActivity::handleButtons();
}

bool OpdsSettingsActivity::saveServer() {
  bool success = false;

  if (isNewServer) {
    // Create flow: first save inserts a new server record into the multi-server store.
    success = OPDS_STORE.addServer(editServer);
    if (success) {
      // After the first successful save, promote to an existing server so
      // subsequent field edits update in-place rather than creating duplicates.
      isNewServer = false;
      serverIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
    } else {
      LOG_ERR("OPS", "Failed to add OPDS server");
    }
  } else {
    // Edit flow: update the same server entry in-place.
    success = OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
    if (!success) {
      LOG_ERR("OPS", "Failed to update OPDS server at index %d", serverIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }

  return success;
}

void OpdsSettingsActivity::handleSelection() {
  // Each field edit is saved immediately so partially configured servers
  // survive navigation and power-loss scenarios.
  if (nav.selected == 0) {
    // Server Name
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.name = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultNoThrow<KeyboardEntryActivity>(handler, renderer, mappedInput, tr(STR_SERVER_NAME),
                                                         editServer.name, 63, InputType::Text);
  } else if (nav.selected == 1) {
    // Server URL
    const std::string prefillUrl = editServer.url.empty() ? "https://" : editServer.url;
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultNoThrow<KeyboardEntryActivity>(handler, renderer, mappedInput, tr(STR_OPDS_SERVER_URL),
                                                         prefillUrl, 127, InputType::Url);
  } else if (nav.selected == 2) {
    // Username
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.username = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultNoThrow<KeyboardEntryActivity>(handler, renderer, mappedInput, tr(STR_USERNAME),
                                                         editServer.username, 63, InputType::Text);
  } else if (nav.selected == PASSWORD_ROW) {
    // Password
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.password = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    // Opens EMPTY, never prefilled with the stored password. Passwords are XOR'd with this
    // chip's factory MAC (ObfuscationUtils.cpp:23), so a store copied from another device
    // decodes to unprintable bytes -- and prefilling those made the field unfixable: the
    // junk renders as blanks, the user types onto the end of it, and the save keeps both.
    // An empty field also means a retype always fully replaces what is stored.
    startActivityForResultNoThrow<KeyboardEntryActivity>(handler, renderer, mappedInput, tr(STR_PASSWORD), "", 63,
                                                         InputType::Text);
  } else if (nav.selected == 4) {
    // Sort A-Z toggle: flip in place and persist.
    editServer.sortAlphabetical = !editServer.sortAlphabetical;
    saveServer();
    requestUpdate();
  } else if (nav.selected == 5) {
    // Extra query: appended to every feed fetch (e.g. "limit=20"). No leading '?'.
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.extraQuery = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResultNoThrow<KeyboardEntryActivity>(handler, renderer, mappedInput, tr(STR_OPDS_EXTRA_QUERY),
                                                         editServer.extraQuery, 63, InputType::Text);
  } else if (nav.selected == 6 && !isNewServer) {
    // Delete flow is only available for existing servers. Confirm first so a
    // mis-press on this row can't silently destroy a configured server.
    const int idx = serverIndex;
    const std::string& body = editServer.name.empty() ? editServer.url : editServer.name;
    startActivityForResultNoThrow<ConfirmationActivity>(
        [this, idx](const ActivityResult& res) {
          if (res.isCancelled) return;
          if (!OPDS_STORE.removeServer(static_cast<size_t>(idx))) {
            LOG_ERR("OPS", "Failed to remove OPDS server at index %d", idx);
            showSaveError = true;
            requestUpdate();
            return;
          }
          finish();
        },
        renderer, mappedInput, tr(STR_DELETE_SERVER), body);
  }
}

void OpdsSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints; derived
  // from the safe area so board bezel insets apply (same as LanguageSelect).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});

  // URL hint where the old sub-header band sat.
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), tr(STR_CALIBRE_URL_HINT), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // fieldRowItems' labels/actionValue were set once in the constructor; only
  // the live value pointers (already pointing at editServer's own fields, no
  // new strings built) need refreshing here.
  fieldRowItems[0].value = editServer.name.empty() ? tr(STR_NOT_SET) : editServer.name.c_str();
  fieldRowItems[1].value = editServer.url.empty() ? tr(STR_NOT_SET) : editServer.url.c_str();
  fieldRowItems[2].value = editServer.username.empty() ? tr(STR_NOT_SET) : editServer.username.c_str();
  // Revealed only while the Password row is ALSO the selected row: moving the selection
  // away hides the password again, with no extra state to clear.
  const bool showPassword = revealPassword && nav.selected == PASSWORD_ROW && !editServer.password.empty();
  fieldRowItems[PASSWORD_ROW].value = editServer.password.empty() ? tr(STR_NOT_SET)
                                      : showPassword              ? editServer.password.c_str()
                                                                  : "******";
  // LOCAL(feat): the two feat-only rows. I18N.get() returns a stable pointer
  // and extraQuery is a member, so both outlive the row list.
  fieldRowItems[4].value = I18N.get(editServer.sortAlphabetical ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF);
  fieldRowItems[5].value = editServer.extraQuery.empty() ? tr(STR_NOT_SET) : editServer.extraQuery.c_str();

  fui::ListProps props;
  props.items = fieldRowItems;
  props.count = static_cast<uint16_t>(getMenuItemCount());
  props.action = ACTION_ROW;
  // Tap opens the row; a touch hold on the Password row reveals it (onRowLongPress).
  // ListItem carries no per-row mask, so the other rows are filtered in the handler
  // instead. Physical buttons stay in loop().
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  // No valueInset: sidePadding already insets both edges of the row, so any
  // extra here lands on the trailing side only and the value sits further
  // from the edge than the label does.
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

const char* OpdsSettingsActivity::headerTitle() const {
  // Reuse STR_OPDS_BROWSER as the "edit existing server" title.
  // New server creation uses STR_ADD_SERVER.
  return isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_BROWSER);
}

void OpdsSettingsActivity::drawFooter() {
  UiListActivity::drawFooter();
  if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }
}
