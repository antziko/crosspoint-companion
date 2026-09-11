#pragma once

#include "OpdsServerStore.h"
#include "activities/UiListActivity.h"

/**
 * Edit screen for a single OPDS server.
 * Shows Name, URL, Username, Password fields and a Delete option.
 * Used for both adding new servers and editing existing ones.
 */
class OpdsSettingsActivity final : public UiListActivity {
 public:
  /**
   * @param serverIndex Index into OpdsServerStore, or -1 for a new server
   */
  explicit OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1);

  void onEnter() override;

 private:
  int serverIndex;
  OpdsServer editServer;
  bool isNewServer = false;
  bool showSaveError = false;
  // Password reveal. Shown only while the Password row is also the selected row, so moving
  // off it hides the password again without any extra bookkeeping; cleared outright when a
  // row is activated. Never persisted and never restored — a reveal dies with the screen.
  bool revealPassword = false;
  bool revealHoldFired = false;     // swallow the Confirm release that ended a reveal hold
  bool confirmPressActive = false;  // true only when a Confirm press originated inside this activity

  int listCount() const override { return getMenuItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Hold the Password row to reveal the stored password (touch).
  void onRowLongPress(int index) override;
  // Hold Confirm on the Password row to do the same on a board with buttons.
  bool handleButtons() override;
  const char* headerTitle() const override;
  void drawFooter() override;

  int getMenuItemCount() const;
  void togglePasswordReveal();
  void handleSelection();
  bool saveServer();

  // Row storage: at most 7 rows (Name/URL/Username/Password/Sort A-Z/Extra
  // query + Delete, see BASE_ITEMS in the .cpp), so a fixed-capacity array
  // avoids any heap allocation for the row list. Labels are set once in the
  // constructor (they never change); buildScreen() only refreshes the value
  // pointers, which already point at editServer's own fields (no new strings).
  // 7, not upstream's 5: feat has the two extra fields above, and undersizing
  // this overruns fieldRowItems.
  static constexpr int MAX_MENU_ITEMS = 7;
  freeink::ui::ListItem fieldRowItems[MAX_MENU_ITEMS]{};
};
