#include "ClockSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstdio>
#include <memory>
#include <string>

#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum MenuItem {
  ITEM_MODE = 0,
  ITEM_FORMAT,
  ITEM_UTC_OFFSET,
  ITEM_AUTO_SYNC,
  ITEM_SHOW_HOME_SYNC,
  ITEM_SYNC_NOW,
  ITEM_COUNT
};

const StrId menuNames[ITEM_COUNT] = {
    StrId::STR_CLOCK,           StrId::STR_CLOCK_FORMAT,         StrId::STR_CLOCK_UTC_OFFSET,
    StrId::STR_CLOCK_AUTO_SYNC, StrId::STR_CLOCK_SHOW_HOME_SYNC, StrId::STR_CLOCK_SYNC_NOW,
};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};

const StrId clockModeNames[CrossPointSettings::CLOCK_MODE_COUNT] = {
    StrId::STR_CLOCK_MODE_OFF,
    StrId::STR_CLOCK_MODE_RTC,
    StrId::STR_CLOCK_MODE_RAM,
};

// Cycle clock mode, skipping CLOCK_RTC on devices without a DS3231.
uint8_t nextClockMode(uint8_t current, bool hasRtc) {
  uint8_t next = (current + 1) % CrossPointSettings::CLOCK_MODE_COUNT;
  if (!hasRtc && next == CrossPointSettings::CLOCK_RTC) {
    next = (next + 1) % CrossPointSettings::CLOCK_MODE_COUNT;
  }
  return next;
}

std::string formatUtcOffset(uint8_t biasedQ) {
  if (biasedQ > 104) biasedQ = 48;
  int totalMinutes = (static_cast<int>(biasedQ) - 48) * 15;
  bool neg = totalMinutes < 0;
  int absMinutes = neg ? -totalMinutes : totalMinutes;
  int hours = absMinutes / 60;
  int mins = absMinutes % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "UTC%c%d:%02d", neg ? '-' : '+', hours, mins);
  return buf;
}
}  // namespace

void ClockSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  // Same clamps as before — defensive against corrupt SPIFFS or hardware swap.
  if (SETTINGS.statusBarClock >= CrossPointSettings::CLOCK_MODE_COUNT) {
    SETTINGS.statusBarClock = CrossPointSettings::CLOCK_OFF;
  }
  if (!halClock.hasRtc() && SETTINGS.statusBarClock == CrossPointSettings::CLOCK_RTC) {
    SETTINGS.statusBarClock = CrossPointSettings::CLOCK_OFF;
  }
  if (SETTINGS.clockUtcOffsetQ > 104) {
    SETTINGS.clockUtcOffsetQ = 48;
  }
  if (SETTINGS.clockFormat >= CLOCK_FORMAT_ITEMS) {
    SETTINGS.clockFormat = 0;
  }

  rebuildVisibleItems();
  requestUpdate();
}

void ClockSettingsActivity::rebuildVisibleItems() {
  visibleItems.clear();
  visibleItems.reserve(ITEM_COUNT);
  visibleItems.push_back(ITEM_MODE);
  visibleItems.push_back(ITEM_FORMAT);
  visibleItems.push_back(ITEM_UTC_OFFSET);
  // Auto-sync only seeds the RTC; RAM mode loses time at every reboot so a "sync on boot"
  // toggle has no meaning. Hide it outside of RTC mode.
  if (SETTINGS.statusBarClock == CrossPointSettings::CLOCK_RTC) {
    visibleItems.push_back(ITEM_AUTO_SYNC);
  }
  visibleItems.push_back(ITEM_SHOW_HOME_SYNC);
  visibleItems.push_back(ITEM_SYNC_NOW);
}

void ClockSettingsActivity::onExit() { Activity::onExit(); }

void ClockSettingsActivity::loop() {
  const int n = static_cast<int>(visibleItems.size());

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, n] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, n);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, n] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, n);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, n] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, n);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, n] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, n);
    requestUpdate();
  });
}

void ClockSettingsActivity::handleSelection() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(visibleItems.size())) return;
  const uint8_t item = visibleItems[selectedIndex];
  switch (item) {
    case ITEM_MODE:
      SETTINGS.statusBarClock = nextClockMode(SETTINGS.statusBarClock, halClock.hasRtc());
      // Mode change may show/hide ITEM_AUTO_SYNC. Rebuild and clamp selection.
      rebuildVisibleItems();
      if (selectedIndex >= static_cast<int>(visibleItems.size())) {
        selectedIndex = static_cast<int>(visibleItems.size()) - 1;
      }
      break;
    case ITEM_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % CLOCK_FORMAT_ITEMS;
      break;
    case ITEM_UTC_OFFSET:
      startActivityForResult(std::make_unique<ClockOffsetActivity>(renderer, mappedInput), nullptr);
      return;
    case ITEM_AUTO_SYNC:
      SETTINGS.autoSyncOnBoot = SETTINGS.autoSyncOnBoot ? 0 : 1;
      break;
    case ITEM_SHOW_HOME_SYNC:
      SETTINGS.showNtpSyncOnHome = SETTINGS.showNtpSyncOnHome ? 0 : 1;
      break;
    case ITEM_SYNC_NOW:
      startActivityForResult(std::make_unique<ClockSyncActivity>(renderer, mappedInput), nullptr);
      return;
    default:
      return;
  }
  SETTINGS.saveToFile();
}

void ClockSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const auto& items = visibleItems;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(items.size()), selectedIndex,
      [&items](int index) { return std::string(I18N.get(menuNames[items[index]])); }, nullptr, nullptr,
      [&items](int index) -> std::string {
        switch (items[index]) {
          case ITEM_MODE: {
            const uint8_t mode = SETTINGS.statusBarClock < CrossPointSettings::CLOCK_MODE_COUNT
                                     ? SETTINGS.statusBarClock
                                     : CrossPointSettings::CLOCK_OFF;
            return std::string(I18N.get(clockModeNames[mode]));
          }
          case ITEM_FORMAT: {
            const uint8_t fmt = SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0;
            return std::string(I18N.get(clockFormatNames[fmt]));
          }
          case ITEM_UTC_OFFSET:
            return formatUtcOffset(SETTINGS.clockUtcOffsetQ);
          case ITEM_AUTO_SYNC:
            return SETTINGS.autoSyncOnBoot ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
          case ITEM_SHOW_HOME_SYNC:
            return SETTINGS.showNtpSyncOnHome ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
          case ITEM_SYNC_NOW:
            return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNCED) : tr(STR_NOT_SET);
          default:
            return std::string();
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
