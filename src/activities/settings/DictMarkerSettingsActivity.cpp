#include "DictMarkerSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
enum MenuItem {
  ITEM_ENABLE = 0,
  ITEM_T1,
  ITEM_T2,
  ITEM_COUNT,
};

const StrId menuNames[ITEM_COUNT] = {StrId::STR_DICT_MARKER_DWELL, StrId::STR_DICT_MARKER_T1,
                                     StrId::STR_DICT_MARKER_T2};

// Labels for each threshold index, parallel to CrossPointSettings::DICT_MARKER_T1/T2_SECONDS.
constexpr int T1_ITEMS = 4;
const StrId t1Names[T1_ITEMS] = {StrId::STR_SEC_3, StrId::STR_SEC_5, StrId::STR_SEC_8, StrId::STR_SEC_10};
constexpr int T2_ITEMS = 4;
const StrId t2Names[T2_ITEMS] = {StrId::STR_SEC_9, StrId::STR_SEC_12, StrId::STR_SEC_15, StrId::STR_SEC_18};
}  // namespace

void DictMarkerSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  // Clamp possibly-corrupt indices so the label lookups below stay in range.
  if (SETTINGS.dictMarkerT1Idx >= T1_ITEMS) SETTINGS.dictMarkerT1Idx = 0;
  if (SETTINGS.dictMarkerT2Idx >= T2_ITEMS) SETTINGS.dictMarkerT2Idx = 0;
  requestUpdate();
}

void DictMarkerSettingsActivity::onExit() { Activity::onExit(); }

void DictMarkerSettingsActivity::loop() {
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

void DictMarkerSettingsActivity::handleSelection() {
  switch (selectedIndex) {
    case ITEM_ENABLE:
      SETTINGS.dictMarkerDwellEnabled = (SETTINGS.dictMarkerDwellEnabled + 1) % 2;
      break;
    case ITEM_T1:
      SETTINGS.dictMarkerT1Idx = (SETTINGS.dictMarkerT1Idx + 1) % T1_ITEMS;
      break;
    case ITEM_T2:
      SETTINGS.dictMarkerT2Idx = (SETTINGS.dictMarkerT2Idx + 1) % T2_ITEMS;
      break;
    default:
      return;
  }
  SETTINGS.saveToFile();
}

void DictMarkerSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICT_MARKER_SETTINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ITEM_COUNT, selectedIndex,
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) -> std::string {
        switch (index) {
          case ITEM_ENABLE:
            return SETTINGS.dictMarkerDwellEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
          case ITEM_T1:
            return std::string(I18N.get(t1Names[SETTINGS.dictMarkerT1Idx < T1_ITEMS ? SETTINGS.dictMarkerT1Idx : 0]));
          case ITEM_T2:
            return std::string(I18N.get(t2Names[SETTINGS.dictMarkerT2Idx < T2_ITEMS ? SETTINGS.dictMarkerT2Idx : 0]));
          default:
            return "";
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
