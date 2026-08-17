#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <cstring>
#include <memory>

#include "ClockOffsetActivity.h"
#include "ClockSyncActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// Menu items in their natural order. Clock entries are appended only when the
// device can have a clock — X3 (DS3231 RTC) or X4 (NTP over WiFi). Devices that
// can have neither don't see them at all (see deviceCanHaveClock in onEnter).
enum MenuItem {
  ITEM_CHAPTER_PAGE_COUNT = 0,
  ITEM_BOOK_PROGRESS_PERCENTAGE,
  ITEM_PROGRESS_BAR,
  ITEM_PROGRESS_BAR_THICKNESS,
  ITEM_TITLE,
  ITEM_BATTERY,
  ITEM_XTC_STATUS_BAR,
  ITEM_CLOCK,             // clock-capable only
  ITEM_CLOCK_FORMAT,      // clock-capable only
  ITEM_CLOCK_UTC_OFFSET,  // clock-capable only, launches ClockOffsetActivity
  ITEM_CLOCK_SYNC,        // clock-capable only, launches ClockSyncActivity
  ITEM_DATE,              // clock-capable only
  ITEM_DATE_FORMAT,       // clock-capable only
  ITEM_COUNT
};

constexpr int BASE_MENU_ITEMS = ITEM_CLOCK;  // Items shown on every device
constexpr int FULL_MENU_ITEMS = ITEM_COUNT;  // Items shown when the device can have a clock
static_assert(FULL_MENU_ITEMS == StatusBarSettingsActivity::MAX_STATUS_BAR_ITEMS,
              "keep StatusBarSettingsActivity::MAX_STATUS_BAR_ITEMS in sync with ITEM_COUNT");

const StrId menuNames[FULL_MENU_ITEMS] = {
    StrId::STR_CHAPTER_PAGE_COUNT,
    StrId::STR_BOOK_PROGRESS_PERCENTAGE,
    StrId::STR_PROGRESS_BAR,
    StrId::STR_PROGRESS_BAR_THICKNESS,
    StrId::STR_TITLE,
    StrId::STR_BATTERY,
    StrId::STR_XTC_STATUS_BAR,
    StrId::STR_CLOCK,
    StrId::STR_CLOCK_FORMAT,
    StrId::STR_CLOCK_UTC_OFFSET,
    StrId::STR_CLOCK_SYNC_NOW,
    StrId::STR_DATE,
    StrId::STR_DATE_FORMAT,
};

constexpr int CLOCK_FORMAT_ITEMS = 2;
const StrId clockFormatNames[CLOCK_FORMAT_ITEMS] = {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H};

constexpr int DATE_FORMAT_ITEMS = 4;
const StrId dateFormatNames[DATE_FORMAT_ITEMS] = {
    StrId::STR_DATE_FMT_0,
    StrId::STR_DATE_FMT_1,
    StrId::STR_DATE_FMT_2,
    StrId::STR_DATE_FMT_3,
};

std::string formatUtcOffset(uint8_t biasedQ) {
  // biasedQ is in quarter-hour steps, biased by 48 (so 48 = UTC+0).
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
constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int TITLE_ITEMS = 3;
const StrId titleNames[TITLE_ITEMS] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};

constexpr int XTC_STATUS_BAR_ITEMS = 3;
const StrId xtcStatusBarNames[XTC_STATUS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP};

// NOTE: upstream renders this row as a 3-way Hide/Right/Left choice off
// STATUS_BAR_CLOCK_MODE_COUNT. Not adopted: feat's status bar (BaseTheme.cpp,
// drawStatusBar) treats statusBarClock as a boolean and always lays the clock
// out against the right edge — STATUS_BAR_CLOCK_LEFT is never honoured. Showing
// "Left" here would advertise a placement the renderer cannot produce, so the
// row stays Show/Hide and the toggle stays % 2, consistent with each other.

const int verticalPreviewTextPadding = 40;
}  // namespace

StatusBarSettingsActivity::StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("StatusBarSettings", renderer, mappedInput) {}

void StatusBarSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  // LOCAL(feat): show the clock/date items when the device CAN have a clock: X3
  // (DS3231, always available) or X4 (no hardware RTC, but gets time from NTP over
  // WiFi). The X4 menu must stay visible even before the first sync so the user can
  // enable the clock and trigger a sync — so gate on capability (!hasHardwareRtc),
  // not on upstream's isAvailable() state, which is false on X4 until time arrives
  // and would hide the entire clock section on that board.
  const bool deviceCanHaveClock = halClock.isAvailable() || !halClock.hasHardwareRtc();
  visibleItemCount = deviceCanHaveClock ? FULL_MENU_ITEMS : BASE_MENU_ITEMS;

  // Clamp statusBarProgressBar and statusBarTitle in case of corrupt/migrated data
  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }

  if (SETTINGS.statusBarProgressBarThickness >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarProgressBarThickness = CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }

  if (SETTINGS.statusBarTitle >= TITLE_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  }

  if (SETTINGS.xtcStatusBarMode >= XTC_STATUS_BAR_ITEMS) {
    SETTINGS.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_HIDE;
  }

  if (SETTINGS.clockUtcOffsetQ > 104) {
    SETTINGS.clockUtcOffsetQ = 48;  // Default to UTC+0
  }

  if (SETTINGS.clockFormat >= CLOCK_FORMAT_ITEMS) {
    SETTINGS.clockFormat = 0;
  }

  if (SETTINGS.dateFormat >= DATE_FORMAT_ITEMS) {
    SETTINGS.dateFormat = 0;
  }

  // Labels never change (unlike the values, which track live SETTINGS
  // state), so they're set once here rather than every buildScreen() call.
  for (int i = 0; i < visibleItemCount; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

bool StatusBarSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void StatusBarSettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  nav.selected = index;
  // Activation opens a popup/sub-activity or repaints a new value; a lingering
  // flash would gray an unrelated row.
  app.clearTapFlash();
  handleSelection();
  requestUpdate();
}

void StatusBarSettingsActivity::handleSelection() {
  switch (nav.selected) {
    case ITEM_CHAPTER_PAGE_COUNT:
      SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
      break;
    case ITEM_BOOK_PROGRESS_PERCENTAGE:
      SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
      break;
    case ITEM_PROGRESS_BAR:
      optionPopup.show(StrId::STR_PROGRESS_BAR, progressBarNames, PROGRESS_BAR_ITEMS, SETTINGS.statusBarProgressBar,
                       [this](int idx) {
                         SETTINGS.statusBarProgressBar = idx;
                         SETTINGS.saveToFile();
                       });
      return;
    case ITEM_PROGRESS_BAR_THICKNESS:
      optionPopup.show(StrId::STR_PROGRESS_BAR_THICKNESS, progressBarThicknessNames, PROGRESS_BAR_THICKNESS_ITEMS,
                       SETTINGS.statusBarProgressBarThickness, [this](int idx) {
                         SETTINGS.statusBarProgressBarThickness = idx;
                         SETTINGS.saveToFile();
                       });
      return;
    case ITEM_TITLE:
      optionPopup.show(StrId::STR_TITLE, titleNames, TITLE_ITEMS, SETTINGS.statusBarTitle, [this](int idx) {
        SETTINGS.statusBarTitle = idx;
        SETTINGS.saveToFile();
      });
      return;
    case ITEM_BATTERY:
      SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
      break;
    case ITEM_XTC_STATUS_BAR:
      optionPopup.show(StrId::STR_XTC_STATUS_BAR, xtcStatusBarNames, XTC_STATUS_BAR_ITEMS, SETTINGS.xtcStatusBarMode,
                       [this](int idx) {
                         SETTINGS.xtcStatusBarMode = idx;
                         SETTINGS.saveToFile();
                       });
      return;
    case ITEM_CLOCK:
      SETTINGS.statusBarClock = (SETTINGS.statusBarClock + 1) % 2;
      break;
    case ITEM_CLOCK_FORMAT:
      SETTINGS.clockFormat = (SETTINGS.clockFormat + 1) % CLOCK_FORMAT_ITEMS;
      break;
    case ITEM_CLOCK_UTC_OFFSET:
      // Launch the dedicated offset picker. It saves on exit, no result handler needed.
      startActivityForResultNoThrow<ClockOffsetActivity>(nullptr, renderer, mappedInput);
      return;
    case ITEM_CLOCK_SYNC:
      startActivityForResultNoThrow<ClockSyncActivity>(nullptr, renderer, mappedInput);
      return;
    case ITEM_DATE:
      SETTINGS.statusBarDate = (SETTINGS.statusBarDate + 1) % 2;
      break;
    case ITEM_DATE_FORMAT:
      SETTINGS.dateFormat = (SETTINGS.dateFormat + 1) % DATE_FORMAT_ITEMS;
      break;
    default:
      return;
  }
  SETTINGS.saveToFile();
}

std::string StatusBarSettingsActivity::rowValueText(const int index) {
  switch (index) {
    case ITEM_CHAPTER_PAGE_COUNT:
      return SETTINGS.statusBarChapterPageCount ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_BOOK_PROGRESS_PERCENTAGE:
      return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_PROGRESS_BAR:
      return I18N.get(progressBarNames[SETTINGS.statusBarProgressBar]);
    case ITEM_PROGRESS_BAR_THICKNESS:
      return I18N.get(progressBarThicknessNames[SETTINGS.statusBarProgressBarThickness]);
    case ITEM_TITLE:
      return I18N.get(titleNames[SETTINGS.statusBarTitle]);
    case ITEM_BATTERY:
      return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_XTC_STATUS_BAR:
      return I18N.get(xtcStatusBarNames[SETTINGS.xtcStatusBarMode]);
    case ITEM_CLOCK:
      return SETTINGS.statusBarClock ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_CLOCK_FORMAT: {
      const uint8_t fmt = SETTINGS.clockFormat < CLOCK_FORMAT_ITEMS ? SETTINGS.clockFormat : 0;
      return std::string(I18N.get(clockFormatNames[fmt]));
    }
    case ITEM_CLOCK_UTC_OFFSET:
      return formatUtcOffset(SETTINGS.clockUtcOffsetQ);
    case ITEM_CLOCK_SYNC:
      return SETTINGS.clockHasBeenSynced ? tr(STR_CLOCK_SYNCED) : tr(STR_NOT_SET);
    // LOCAL(feat): the two date rows have no upstream equivalent, so upstream's
    // rowValueText() omits them. Without these cases both fall to the default
    // below and render "Hide" no matter what the setting actually is.
    case ITEM_DATE:
      return SETTINGS.statusBarDate ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_DATE_FORMAT: {
      const uint8_t fmt = SETTINGS.dateFormat < DATE_FORMAT_ITEMS ? SETTINGS.dateFormat : 0;
      return std::string(I18N.get(dateFormatNames[fmt]));
    }
    default:
      return tr(STR_HIDE);
  }
}

void StatusBarSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Reserve the bottom band for the live status-bar preview footer (label +
  // bar) so the list never runs underneath it, plus the button-hints row below.
  // The preview is pinned directly above the hints (see render()), so the band
  // is just the bar + its label, not a floating gap.
  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const auto previewFooter =
      static_cast<int16_t>(statusBarHeight + verticalPreviewTextPadding + metrics.verticalSpacing);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight + previewFooter), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems_'s labels/actionValue were set once in onEnter(); only the live
  // value text needs refreshing here, by assigning into the existing
  // rowValues_ strings (no array growth) rather than building a new
  // items/values vector on every render.
  for (int i = 0; i < visibleItemCount; i++) {
    rowValues_[i] = rowValueText(i);
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(visibleItemCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;  // also the explicitly-set marker, see SettingsActivity
  syncListViewport(screen, props);
  screen.list(props);
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  // Through renderListFrame: this screen's rows set labelText.maxLines = 2, so
  // a wrapped row is taller than the estimate and the list's layout feedback
  // can move the viewport mid-build (see UiListActivity::renderListFrame).
  renderListFrame(
      [](void* ctx) {
        auto* self = static_cast<StatusBarSettingsActivity*>(ctx);
        self->renderer.clearScreen();
        const auto m = UITheme::getInstance().getMetrics();
        // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
        // indicator; the list renders through the app; the preview stays raw.
        GUI.drawHeader(self->renderer, Rect{0, m.topPadding, self->renderer.getScreenWidth(), m.headerHeight},
                       tr(STR_CUSTOMISE_STATUS_BAR));
        self->renderUi();
      },
      this);

  auto metrics = UITheme::getInstance().getMetrics();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  std::string title;
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = tr(STR_EXAMPLE_BOOK);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_EXAMPLE_CHAPTER);
  }

  // Anchor the preview as a footer directly above the button hints.
  GUI.drawStatusBar(renderer, 75, 8, 32, title, metrics.buttonHintsHeight, 0, false);

  renderer.drawCenteredText(UI_10_FONT_ID,
                            renderer.getScreenHeight() - UITheme::getInstance().getStatusBarHeight() -
                                metrics.buttonHintsHeight - verticalPreviewTextPadding,
                            tr(STR_PREVIEW));

  renderer.displayBuffer();
}
