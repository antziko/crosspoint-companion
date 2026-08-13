#pragma once
#include <Epub.h>

#include <memory>
#include <string>
#include <vector>

#include "../../BookmarkStore.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderBookmarksActivity final : public UiListActivity {
  // The list rides the UiListActivity scaffold (themed rows, touch routing,
  // swipe scrolling); OptionPopup provides the delete confirmation, replacing
  // the legacy two-press confirmingDelete state machine.
  std::string epubPath;
  std::vector<Bookmark> bookmarks;
  // Row buffers derived from `bookmarks`, rebuilt only when it changes
  // (onEnter() load, post-delete) instead of on every repaint. The legacy
  // screen re-wrapped every quote snippet and re-composed a percentage /
  // page / chapter subtitle string for every row on each render — including
  // plain cursor moves and tap flashes.
  std::vector<std::string> rowLabels;
  std::vector<std::string> rowSubtitles;
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();

  OptionPopup confirmPopup;
  // True while the button press that closed the popup is still held; its
  // release must not fall through to the list's own Back/Confirm handlers.
  bool popupClosing = false;

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::string& epubPath);
  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return static_cast<int>(bookmarks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Popup handling runs before everything else each pass.
  bool handleCustomInput() override;
  // Back cancels with a result; Confirm opens on RELEASE, a hold deletes.
  bool handleButtons() override;
  void drawChrome() override;
  void drawFooter() override;

  // Open the selected bookmark. Quote rows open QuoteViewerActivity (which can
  // forward a jump of its own); point bookmarks finish with a BookmarkResult.
  void openSelectedBookmark();
  // Cancel/Delete confirmation for the selected bookmark; shared by the
  // physical Confirm hold and the touch row long-press.
  void showDeleteConfirmation();
  void deleteSelectedBookmark();
};
