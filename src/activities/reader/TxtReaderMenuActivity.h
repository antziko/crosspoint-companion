#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/ListTouchTarget.h"
#include "util/ButtonNavigator.h"

// In-reader menu for the plain-text (.txt) reader. A deliberately slim sibling
// of EpubReaderMenuActivity: only the actions that make sense for a structureless
// text stream (no chapters, footnotes, dictionary, sync, or reading stats).
//
// Returns a MenuResult carrying the chosen action plus the (possibly cycled)
// orientation and auto-page-turn option, which the reader applies on exit.
class TxtReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction {
    READER_OPTIONS,
    VIEW_BOOKMARKS,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    DELETE_CACHE,
    SCREENSHOT,
    DISPLAY_QR
  };

  explicit TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                 int currentPage, int totalPages, int progressPercent, uint8_t currentOrientation,
                                 uint8_t currentPageTurnOption);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems();

  const std::vector<MenuItem> menuItems;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  std::string title;
  int currentPage = 0;
  int totalPages = 0;
  int progressPercent = 0;

  // Orientation is no longer rotated from this menu (it's a side-button hold
  // gesture); kept only to round-trip the current value back to the reader.
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;

  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};

  // Rows the last render drew, so a tap can pick one (see ListTouchTarget).
  ListTouchTarget listTouch_;
};
