#include "TxtReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

std::vector<TxtReaderMenuActivity::MenuItem> TxtReaderMenuActivity::buildMenuItems() {
  return {
      {MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS},
      {MenuAction::VIEW_BOOKMARKS, StrId::STR_BOOKMARKS},
      {MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT},
      {MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN},
      {MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE},
      {MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON},
      {MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR},
  };
}

TxtReaderMenuActivity::TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                             int currentPage, int totalPages, int progressPercent,
                                             uint8_t currentOrientation, uint8_t currentPageTurnOption)
    : Activity("TxtReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems()),
      title(std::move(title)),
      currentPage(currentPage),
      totalPages(totalPages),
      progressPercent(progressPercent),
      pendingOrientation(currentOrientation),
      selectedPageTurnOption(currentPageTurnOption) {}

void TxtReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void TxtReaderMenuActivity::onExit() { Activity::onExit(); }

void TxtReaderMenuActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  // A tap on a row selects and activates it in one go, like the FUI list screens.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.touchRow(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0) selectedIndex = tappedRow;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || tappedRow >= 0) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption};
    setResult(std::move(result));
    finish();
    return;
  }
}

void TxtReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  std::string progressLine = std::to_string(currentPage) + "/" + std::to_string(totalPages) +
                             std::string(tr(STR_PAGES_SEPARATOR)) + std::to_string(progressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  listTouch_.record(Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(menuItems.size()),
                    selectedIndex);
  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) -> std::string {
        if (menuItems[index].action == MenuAction::AUTO_PAGE_TURN) {
          return pageTurnLabels[selectedPageTurnOption];
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
