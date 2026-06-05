#pragma once

#include "KOReaderCredentialStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Edit screen for a single KOReader sync server.
 * Used for both adding new servers (serverIndex = -1) and editing existing ones.
 *
 * Rows for new servers:    Name, Username, Password, Sync Server URL, Doc Matching
 * Rows for existing:       + Authenticate (success makes this the sync server)
 * Delete row shown when:   existing server AND total count > 1
 */
class KOReaderSettingsActivity final : public Activity {
 public:
  /**
   * @param serverIndex Index into KOReaderCredentialStore, or -1 for a new server.
   */
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1)
      : Activity("KOReaderSettings", renderer, mappedInput), serverIndex(serverIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;
  int serverIndex;
  KOReaderSyncServer editServer;
  bool isNewServer = false;
  bool showSaveError = false;

  int getMenuItemCount() const;
  void handleSelection();
  bool saveServer();
};
