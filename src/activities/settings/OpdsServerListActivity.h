#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

/**
 * Activity showing the list of configured OPDS servers.
 * Allows adding new servers and editing/deleting existing ones.
 * When pickerMode is true, selecting a server navigates to the OPDS browser
 * instead of opening the editor (used from the home screen).
 */
class OpdsServerListActivity final : public UiListActivity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return getItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Touch hold on a server row duplicates it — the buttonless boards' only
  // route to the gesture handleButtons() offers on Confirm.
  void onRowLongPress(int index) override;
  // Popup input goes first; while it is open it consumes the pass.
  bool handleCustomInput() override;
  // Picker mode backs out to the home menu rather than finishing.
  void onBackButton() override;
  // LOCAL(feat): Confirm is handled here rather than by the base, for the
  // hold-to-duplicate gesture and the press-originated-here guard.
  bool handleButtons() override;
  const char* headerTitle() const override;

  bool pickerMode = false;
  bool longPressFired = false;      // Swallow Confirm release after a hold-duplicate fires
  bool confirmPressActive = false;  // True only when a Confirm press originated inside this activity

  OptionPopup optionPopup;

  // Row structure (labels, actionValue), rebuilt only when the server list
  // itself reloads (rebuildRowItems(), called from onEnter() and after
  // returning from the server editor) — not on every repaint. The format
  // row's live subtitle is refreshed in place by buildScreen().
  std::vector<freeink::ui::ListItem> rowItems_;
  // Backing storage for the server rows' "name - url" labels; rowItems_ holds
  // pointers into these, so it must not outlive or be rebuilt without them.
  std::vector<std::string> serverLabels_;
  void rebuildRowItems();

  int getItemCount() const;
  void handleSelection();
  void duplicateSelectedServer();
};
