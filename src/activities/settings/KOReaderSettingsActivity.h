#pragma once

#include <string>

#include "KOReaderCredentialStore.h"
#include "activities/UiListActivity.h"

/**
 * Edit screen for a single KOReader sync server.
 * Used for both adding new servers (serverIndex = -1) and editing existing ones.
 *
 * Rows for new servers:    Name, Username, Password, Sync Server URL, Doc Matching,
 *                          Send Metadata, Sync Behavior
 * Rows for existing:       + Set as Active, Sign Up, Authenticate
 * Delete row shown when:   existing server AND total count > 1
 */
class KOReaderSettingsActivity final : public UiListActivity {
 public:
  /**
   * @param serverIndex Index into KOReaderCredentialStore, or -1 for a new server.
   */
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1);

  void onEnter() override;

  // LOCAL(feat): 11, not upstream's 8 — feat's multi-server editor adds Name,
  // Set as Active and Delete Server on top of upstream's single-server rows.
  // Sizing this to 8 would overrun rowItems_/rowValues_ on an existing server.
  static constexpr int MAX_MENU_ITEMS = 11;

 private:
  int listCount() const override { return getMenuItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void drawFooter() override;

  int serverIndex;
  KOReaderSyncServer editServer;
  bool isNewServer = false;
  bool showSaveError = false;

  // Row storage: MAX_MENU_ITEMS is a compile-time constant, so fixed-capacity
  // storage avoids any heap allocation for the row list. Labels are set once
  // in the constructor; buildScreen() only refreshes the live value text
  // (rowValues_) by assigning into the existing strings (no array growth).
  std::string rowValues_[MAX_MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MAX_MENU_ITEMS]{};

  int getMenuItemCount() const;
  void handleSelection();
  bool saveServer();
};
