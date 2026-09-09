#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderMenuActivity final : public UiListActivity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BOOKMARK_TOGGLE,
    VIEW_BOOKMARKS,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    DELETE_CACHE,
    REPAGINATE,
    LOOKUP,
    LOOKUP_HISTORY,
    REVIEW_FLASHCARDS,
    FLASHCARDS_LIST,
    SET_BOOK_DICTIONARY,
    READER_OPTIONS,
    BOOK_STATS,
    ADD_HIGHLIGHT,
    FRONTLIGHT
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const bool hasDictionary = false, std::string activeDictName = "");

  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

  // Public so the toolbar reader menu's "More" panel can reuse the same row set
  // rather than maintaining a second copy of it (EpubReaderActivity::buildMoreActions).
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  // LOCAL(feat): gated on hasDictionary, not upstream's hasBookmarks. feat's
  // reader menu is a different information architecture — Bookmarks is always
  // present, text settings/rotation live behind READER_OPTIONS, and three
  // dictionary rows (lookup history, flashcard review, flashcard list) have no
  // upstream equivalent. Only upstream's FUI row plumbing is taken here.
  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes, bool hasDictionary);

 private:
  // Row storage: menuItems is at most MAX_MENU_ITEMS (11 always-present rows +
  // FOOTNOTES + the 3 dictionary rows in buildMenuItems() + FRONTLIGHT on
  // boards that have one), so a fixed-capacity array avoids any heap
  // allocation for the row list. Labels are set once in the constructor
  // (buildMenuRowItems()); buildScreen() only refreshes the rows whose value
  // reflects live state (rotation, page-turn interval, dictionary, frontlight).
  static constexpr size_t MAX_MENU_ITEMS = 16;
  freeink::ui::ListItem menuRowItems[MAX_MENU_ITEMS]{};
  void buildMenuRowItems();

  int listCount() const override { return static_cast<int>(menuItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup input/close-swallow runs before any button or touch handling.
  bool handleCustomInput() override;
  // Back closes on RELEASE and Confirm activates on RELEASE; everything else
  // (row navigation, page jumps) falls through to the base handler.
  bool handleButtons() override;
  // Header via GUI.drawHeader inside the safe area for the battery indicator.
  void drawChrome() override;

  void closeCancelled();

  // Fixed menu layout
  const std::vector<MenuItem> menuItems;

  OptionPopup optionPopup;
  // True while the button press that closed the popup is still held; its release
  // must not fall through to the menu's own Back/Confirm handlers.
  bool popupClosing = false;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
  std::string activeDictName;
};
