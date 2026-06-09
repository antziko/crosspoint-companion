#include "HomeTopBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

enum MenuItem {
  ITEM_CLOCK = 0,
  ITEM_DATE,
  ITEM_DATE_FORMAT,
  ITEM_COUNT
};

const StrId menuNames[ITEM_COUNT] = {
    StrId::STR_CLOCK,
    StrId::STR_DATE,
    StrId::STR_DATE_FORMAT,
};

constexpr int DATE_FORMAT_ITEMS = 4;
const StrId dateFormatNames[DATE_FORMAT_ITEMS] = {
    StrId::STR_DATE_FMT_0, StrId::STR_DATE_FMT_1, StrId::STR_DATE_FMT_2, StrId::STR_DATE_FMT_3,
};

}  // namespace

void HomeTopBarSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  if (SETTINGS.homeTopBarDateFormat >= DATE_FORMAT_ITEMS) {
    SETTINGS.homeTopBarDateFormat = 0;
  }
  requestUpdate();
}

void HomeTopBarSettingsActivity::onExit() { Activity::onExit(); }

void HomeTopBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
}

void HomeTopBarSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case ITEM_CLOCK:
      SETTINGS.homeTopBarClock = (SETTINGS.homeTopBarClock + 1) % 2;
      break;
    case ITEM_DATE:
      SETTINGS.homeTopBarDate = (SETTINGS.homeTopBarDate + 1) % 2;
      break;
    case ITEM_DATE_FORMAT:
      SETTINGS.homeTopBarDateFormat = (SETTINGS.homeTopBarDateFormat + 1) % DATE_FORMAT_ITEMS;
      break;
    default:
      return;
  }
  SETTINGS.saveToFile();
}

void HomeTopBarSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_CUSTOMISE_TOP_BAR));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ITEM_COUNT, selectedIndex,
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) -> std::string {
        switch (index) {
          case ITEM_CLOCK:
            return SETTINGS.homeTopBarClock ? tr(STR_SHOW) : tr(STR_HIDE);
          case ITEM_DATE:
            return SETTINGS.homeTopBarDate ? tr(STR_SHOW) : tr(STR_HIDE);
          case ITEM_DATE_FORMAT: {
            const uint8_t fmt = SETTINGS.homeTopBarDateFormat < DATE_FORMAT_ITEMS ? SETTINGS.homeTopBarDateFormat : 0;
            return std::string(I18N.get(dateFormatNames[fmt]));
          }
          default:
            return tr(STR_HIDE);
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
