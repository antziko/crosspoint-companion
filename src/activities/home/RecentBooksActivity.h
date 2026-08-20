#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/UiListActivity.h"

class RecentBooksActivity final : public UiListActivity {
 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;

 private:
  int listCount() const override { return static_cast<int>(recentBooks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Confirm activates on RELEASE here (a hold is "remove from list"), and Back
  // goes home rather than finishing.
  bool handleButtons() override;
  // No-op: handleButtons() already owns every button this screen navigates
  // with. See the definition for why the base tail double-handled them.
  void navigateButtons() override;
  const char* headerTitle() const override { return tr(STR_MENU_RECENT_BOOKS); }
  void drawFooter() override;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;

  // Title-line font, matched to the OPDS browser's book rows: the touch-target
  // sized body font on touch hardware, the denser small font on X3/X4 (where it
  // also fits more of a long title before the line ellipsizes).
  bool usesBodyLabel() const;

  std::vector<RecentBook> recentBooks;
  // Row buffer, built in loadRecentBooks() (not buildScreen(), which reuses
  // it on every repaint instead of rebuilding a ListItem vector per render).
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();

  // Data loading
  void loadRecentBooks();

  // Move the selected entry up/down one slot, persist, and repaint. Returns false
  // (no-op) at the list boundary. Driven by Left/Right taps — a tap, not a hold,
  // because the X3 front buttons can't sustain a reliable held reading.
  bool moveSelectedUp();
  bool moveSelectedDown();

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);
};
