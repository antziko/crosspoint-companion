#include "FrontlightPanelActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/readerToolbarIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_TILE = 6;  // value = tile index

// iOS-style geometry. The panel is a card hanging from the top of the screen:
// a grabber, full-width slider pills, then a row of quick-setting icons. The
// chrome itself is fui::sheet / fui::sliderRow; these constants only size the
// bands, and computePanelBottom() mirrors them.
constexpr int16_t kPanelSideMargin = 16;
constexpr int16_t kGrabberHeight = 5;      // fui::SheetProps default, mirrored here
constexpr int16_t kSliderRowHeight = 56;   // the pill itself (finger-sized)
constexpr int16_t kQuickControlSize = 48;  // the circle behind each icon
constexpr int16_t kQuickIconSize = 32;
// One percent per press, on the -/+ buttons and on the physical Left/Right keys
// alike (both repeat while held), so a level can be set exactly.
constexpr int BRIGHTNESS_STEP = 1;
// The dimmest setting is 1%, not 0: turning the light off is what the lamp
// button next to the slider is for, so a 0% "on" level would only be a second,
// worse way to reach the same place.
constexpr uint8_t MIN_BRIGHTNESS = 1;

// The tile kind the user placed in grid slot `idx` (Settings -> Controls).
uint8_t tileKind(const int idx) {
  const uint8_t kinds[] = {SETTINGS.panelTile1, SETTINGS.panelTile2, SETTINGS.panelTile3, SETTINGS.panelTile4};
  if (idx < 0 || idx >= static_cast<int>(std::size(kinds))) return CrossPointSettings::PANEL_TILE_COUNT;
  return kinds[idx];
}

// Reader action for an action tile; Ignore for the panel's own toggles.
HomeButtonAction tileAction(const uint8_t kind) {
  switch (kind) {
    case CrossPointSettings::PANEL_TILE_FOOTNOTES:
      return HomeButtonAction::Footnotes;
    case CrossPointSettings::PANEL_TILE_BOOKMARK:
      return HomeButtonAction::Bookmark;
    case CrossPointSettings::PANEL_TILE_DICTIONARY:
      return HomeButtonAction::Dictionary;
    case CrossPointSettings::PANEL_TILE_SYNC:
      return HomeButtonAction::Sync;
    case CrossPointSettings::PANEL_TILE_READER_MENU:
      return HomeButtonAction::ReaderMenu;
    case CrossPointSettings::PANEL_TILE_STATUS_BAR:
      return HomeButtonAction::ToggleStatusBar;
    case CrossPointSettings::PANEL_TILE_HOME:
      return HomeButtonAction::Home;
    default:
      return HomeButtonAction::Ignore;
  }
}

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FrontlightPanel", renderer, mappedInput), UiAppHost(renderer) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // A stored 0% predates the 1% floor (or came from the web settings): show it
  // as the floor rather than a level the slider can no longer produce. onExit
  // persists that, which is the intent — 0 is not a brightness any more.
  enteredInverted = SETTINGS.screenInverted;

  brightness = std::max(MIN_BRIGHTNESS, Frontlight.brightness());
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  lightOnChanged = false;

  // Seed the touch tile's restore mode from the live setting, so toggling off
  // and back on within this session returns to the mode the user had.
  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
    touchModeRestore = SETTINGS.touchReaderControls;
  }

  // The orientation actually in force: inside an open book that is the book's own, and
  // everywhere else activeOrientation mirrors the global default anyway.
  orientationShown = static_cast<uint8_t>(APP_STATE.activeOrientation % CrossPointSettings::ORIENTATION_COUNT);

  overBook = activityManager.isInReaderContext();

  resetUi();
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_TILE, &FrontlightPanelActivity::onTileEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  requestUpdate();
}

void FrontlightPanelActivity::persistLightSettings() {
  // brightness/warmth are always restored unconditionally on boot (see
  // main.cpp), so they never diverge from SETTINGS at onEnter() — comparing
  // against SETTINGS here only fires on a genuine user change. lightOn has
  // no such guarantee (see lightOnChanged's declaration), so it's gated on
  // the user actually having touched it this session instead.
  const bool changed = SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                       (lightOnChanged && SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (changed) {
    SETTINGS.frontlightBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    if (lightOnChanged) SETTINGS.frontlightOn = lightOn ? 1 : 0;
    SETTINGS.saveToFile();
  }
}

void FrontlightPanelActivity::onExit() {
  persistLightSettings();
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = std::max(MIN_BRIGHTNESS, percentFromPermille(event.dragPermille));
  Frontlight.setBrightness(self->brightness);
  if (!self->lightOn) {
    self->lightOn = true;
    self->lightOnChanged = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value);
}

void FrontlightPanelActivity::onTileEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->runTile(event.value);
}

// Fades ink already drawn in the rect to 50% by clearing every other pixel. Neither the bitmap
// nor the text path here has a gray paint (text falls back to solid black).
void FrontlightPanelActivity::dimToHalf(const int x, const int y, const int width, const int height) const {
  for (int py = y; py < y + height; ++py) {
    for (int px = x + ((py & 1) ^ 1); px < x + width; px += 2) {
      renderer.drawPixel(px, py, false);
    }
  }
}

// The panel's own toggles and Home work anywhere; the other reader actions need a book.
bool FrontlightPanelActivity::tileAvailable(const uint8_t kind) const {
  const HomeButtonAction action = tileAction(kind);
  return overBook || action == HomeButtonAction::Ignore || action == HomeButtonAction::Home;
}

void FrontlightPanelActivity::runTile(const int idx) {
  // Every tile either flips a setting the tile itself displays, or leaves the
  // panel. The tap flash (FreeInkApp arms it after this handler returns, and it
  // paints the tapped element with the focused style for the one repaint that
  // follows) would sit on top of that: a tile switched OFF would still draw
  // filled until something else repainted the panel, reading as though the
  // setting were still on. The state change is the feedback here, so drop it.
  app.clearTapFlash();

  const uint8_t kind = tileKind(idx);
  if (!tileAvailable(kind)) return;
  const HomeButtonAction action = tileAction(kind);
  if (action != HomeButtonAction::Ignore) {
    // Close first: the action belongs to the screen underneath, which runs it on the next
    // frame. Queued after close(), whose pop clears any pending Home-key action.
    close();
    mappedInput.queueHomeButtonAction(action);
    return;
  }

  switch (kind) {
    case CrossPointSettings::PANEL_TILE_NIGHT_MODE:  // Night mode (inverted output polarity, applied to the whole UI)
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      SETTINGS.saveToFile();
      // Inversion rewrites every pixel; take the clean waveform so the panel
      // does not keep a ghost of the old polarity.
      cleanRefreshPending = true;
      requestUpdate();
      break;
    case CrossPointSettings::PANEL_TILE_REFRESH:  // Ghost-cleanup refresh of the whole frame
      // Refreshing with the panel still up would clean a frame the user is
      // about to dismiss anyway: drop the panel first and let the repaint of
      // the screen underneath carry the clean waveform instead.
      //
      // HALF, not FULL. Half seeds the OLD plane with the complement of the target so EVERY
      // pixel transitions; Full seeds it white and drives only the black-target pixels,
      // leaving the rest parked in the no-transition cell with stale charge
      // (Uc8279X4Driver.cpp:410-419, which names Half as what the manual force-refresh must
      // use). Under night mode the population Full strands is the white text -- the page
      // shape -- so the flash that is supposed to remove the ghost writes it instead.
      renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH, "tile");
      close();
      break;
    case CrossPointSettings::PANEL_TILE_ORIENTATION: {  // Cycle the reading orientation
      const uint8_t next = static_cast<uint8_t>((orientationShown + 1) % CrossPointSettings::ORIENTATION_COUNT);
      orientationShown = next;
      // Over an open book the rotation belongs to that book alone, the same as the reader
      // menu's own rotate and the side-button hold gesture: the global default is left
      // untouched so this book's choice cannot follow the user into the next one. The
      // reader persists it per book when it adopts the request below. Off the reader
      // there is no book to own it, so the tile is the global default (and the mirror
      // activeOrientation keeps of it outside a book).
      if (!SETTINGS.getReaderOverride().active) {
        SETTINGS.orientation = next;
        APP_STATE.activeOrientation = next;
        SETTINGS.saveToFile();
      }
      // The renderer is deliberately NOT turned here: that would crop the sheet
      // and the portrait-only screens the panel opens over. Readers lay out
      // against APP_STATE.activeOrientation (saved per book), so the new value
      // is parked as a one-shot request the reader adopts when the panel closes
      // rather than written straight into the book's own orientation.
      APP_STATE.pendingOrientation = next;
      requestUpdate();
      break;
    }
    case CrossPointSettings::PANEL_TILE_TOUCH:  // Touch reader controls (for reading with the palm on the glass)
      // Toggles the existing Settings -> Controls option, nothing lower-level:
      // that setting only governs the reader's tap/swipe handling, so the
      // panel's own gestures (including the swipe that reopens it) keep
      // working while it is off. Off remembers the mode (Tap/Swipe/Inverted
      // Tap) so toggling back does not stomp the user's choice.
      if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF) {
        touchModeRestore = SETTINGS.touchReaderControls;
        SETTINGS.touchReaderControls = CrossPointSettings::TOUCH_READER_OFF;
      } else {
        SETTINGS.touchReaderControls = touchModeRestore;
      }
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < MIN_BRIGHTNESS) next = MIN_BRIGHTNESS;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    lightOnChanged = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  lightOnChanged = true;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

// The sheet is transient (isTransientScreen), so the entry scrub covers neither opening
// nor closing it -- except where a tile has changed what the frame underneath is going to look
// like. A polarity flip rewrites every pixel and a rotation re-lays the whole screen out; either
// one leaves the outgoing shape behind as stale charge unless the repaint drives every pixel from
// scratch. The refresh tile arms its own promotion before calling here, and when nothing else
// changed this leaves it alone.
void FrontlightPanelActivity::close() {
  const bool polarityChanged = (SETTINGS.screenInverted != 0) != (enteredInverted != 0);
  const bool rotating = APP_STATE.pendingOrientation != CrossPointState::NO_ORIENTATION_REQUEST;
  if (polarityChanged || rotating) {
    renderer.promoteNextRefresh(HalDisplay::SCRUB_REFRESH, polarityChanged ? "polarity" : "rotate");
  }
  finish();
}

bool FrontlightPanelActivity::handleHomeGesture() {
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  // The light can be switched from outside the sheet while it is open: the X4 Pro's
  // power-button double click drives the hardware straight from main.cpp (and skips the
  // activity loop on the frame it fires). Follow the hardware, so the lamp button never
  // contradicts the light the user is looking at. lightOnChanged stays where it is --
  // that path saved SETTINGS.frontlightOn itself, and this is not the user reaching for
  // the panel's own control. Brightness and warmth have no such outside writer.
  if (lightOn != Frontlight.isOn()) {
    lightOn = Frontlight.isOn();
    requestUpdate();
  }

  const auto touch = routeTouch(mappedInput, false, /*routeHeld=*/true);
  if (touch.routed) {
    if (app.invalidated()) requestUpdate();
    if (touch) {
      if (touch.event.dragPermille >= 0) draggingSlider = true;
      return;
    }
    // Swipe up dismisses the sheet, the way it was pulled down from the top
    // edge. draggingSlider keeps a fast slider flick from closing it.
    if (!draggingSlider && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Up) {
      close();
      return;
    }
    // panelBottom > 0 guards the frame the sheet opens in: the release that
    // opened it (a status-bar tap) is still in the input snapshot when the panel
    // runs its first loop(), and panelBottom is only known once render() has
    // measured the layout — so at 0 that release read as "tapped below the
    // sheet" and closed it again before it was ever drawn.
    if (touch.snap.touchReleased && !draggingSlider && panelBottom > 0 && touch.snap.touchY >= panelBottom) {
      close();
      return;
    }
  }
  if (draggingSlider) {
    if (!touch.snap.touchHeld) {
      draggingSlider = false;
      cleanRefreshPending = true;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleLight();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

int FrontlightPanelActivity::computePanelBottom() const {
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lineHeight = uiTarget.lineHeight(tokens.smallText.font);
  int y = tokens.spaceLg + tokens.spaceMd;  // top padding
  if (Frontlight.present()) {
    // Screen::sliderRow reserves caption + spaceMd + control band, then a
    // spaceMd gap; addSliderRow() adds one more spaceMd of air after each row.
    y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // brightness
    if (Frontlight.hasColorTemperature()) {
      y += lineHeight + tokens.spaceMd + kSliderRowHeight + 2 * tokens.spaceMd;  // warmth
    }
    y += tokens.spaceSm;
  }
  // Icons are touch targets, so a buttons-only board gets no row and the
  // sheet is exactly the frontlight controls. addQuickRow() takes the same band.
  if (mappedInput.hasTouch()) y += kQuickControlSize + tokens.spaceSm + lineHeight;
  // The sheet's grabber band: content margin + grabber + air to the edge.
  // buildPanelScreen() feeds the same theme spacings into SheetProps.
  y += tokens.spaceLg + kGrabberHeight + tokens.spaceLg + tokens.spaceMd;
  return y;
}

void FrontlightPanelActivity::panelScreen(UiScreen& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::addSliderRow(UiScreen& screen, const char* label, const uint8_t value,
                                           const fui::ActionId sliderAction, const fui::ActionId stepAction,
                                           const bool showToggle) {
  // Live percentage readout. The row draws before this call returns
  // (immediate mode), so borrowing a stack buffer is safe.
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));

  // rowProps is a member (fui::SliderRowProps embeds a 324-byte StyleSet, well
  // past the 256-byte budget a local gets — AGENTS.md). Every field that
  // varies between the two rows is reassigned here; the rest keep their
  // constructed defaults, which already match the panel's card language.
  rowProps.label = label;
  rowProps.value = pct;
  rowProps.sliderValue = value;
  rowProps.sliderAction = sliderAction;
  rowProps.decrement = stepAction;
  rowProps.increment = stepAction;
  rowProps.decrementValue = -BRIGHTNESS_STEP;
  rowProps.incrementValue = BRIGHTNESS_STEP;
  if (showToggle) {
    // Lamp on/off after the +: the sliders set the level, this kills the light
    // outright. Filled glyph = on, outline = off.
    rowProps.toggleAction = ACTION_TOGGLE;
    rowProps.toggleIcon = fui::bitmapFromIcon(lightOn ? icon_sun_filled_32 : icon_sun_32);
  } else {
    rowProps.toggleAction = fui::NO_ACTION;
    rowProps.toggleIcon = fui::BitmapRef{};
  }
  screen.sliderRow(rowProps, kSliderRowHeight);
  // The wrapper's own trailing gap is one spaceMd; double it so the rows
  // breathe — a control band this tall reads cramped at the list cadence.
  screen.spacer(screen.theme().spaceMd);
}

void FrontlightPanelActivity::buildPanelScreen(UiScreen& screen) {
  const auto& theme = screen.theme();

  // Sheet chrome first: the card body, the 2px rule along its bottom edge, and
  // the grabber on the edge the sheet is dragged from. Screen::sheet() also
  // clamps the content area to the sheet, so every band below lays out inside
  // it. (No header: the panel is a floating card, and its own grabber says
  // what it is.)
  fui::SheetProps sheetProps;
  // A roomy band above the bottom rule: the grabber gets a full spaceLg of
  // air on both sides so the last row of content never crowds the sheet edge.
  sheetProps.grabberMargin = theme.spaceLg;
  sheetProps.grabberInset = static_cast<int16_t>(theme.spaceLg + theme.spaceMd);
  screen.sheet(sheetProps, static_cast<int16_t>(panelBottom));
  screen.insetContent(fui::Insets{0, kPanelSideMargin, 0, kPanelSideMargin});

  // The sheet hangs from the very top of the screen, so its content needs a
  // real top inset of its own — nothing above it reserves space the way a
  // header band did.
  screen.spacer(static_cast<int16_t>(theme.spaceLg + theme.spaceMd));

  if (Frontlight.present()) {
    addSliderRow(screen, tr(STR_BRIGHTNESS), brightness, ACTION_BRIGHTNESS, ACTION_BRIGHTNESS_STEP,
                 /*showToggle=*/true);
    if (Frontlight.hasColorTemperature()) {
      addSliderRow(screen, tr(STR_WARMTH), warmth, ACTION_WARMTH, ACTION_WARMTH_STEP, /*showToggle=*/false);
    }
    screen.spacer(theme.spaceSm);
  }

  if (mappedInput.hasTouch()) addQuickRow(screen);
}

// One row of equal slots, as the reader toolbar lays its icons out: a circle with the tile's
// icon, its name underneath, and the whole slot as the touch target. Touch boards only.
void FrontlightPanelActivity::addQuickRow(UiScreen& screen) {
  const auto& theme = screen.theme();
  static constexpr StrId kOrientNames[CrossPointSettings::ORIENTATION_COUNT] = {
      StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
  static constexpr StrId kTileNames[CrossPointSettings::PANEL_TILE_COUNT] = {
      StrId::STR_NIGHT_MODE,        StrId::STR_FORCE_REFRESH, StrId::STR_ORIENTATION,
      StrId::STR_TOUCH_TOGGLE,      StrId::STR_FOOTNOTES,     StrId::STR_BOOKMARK_OPTION,
      StrId::STR_DICTIONARY,        StrId::STR_KOSYNC,        StrId::STR_HOLD_CONFIRM_READER_MENU,
      StrId::STR_TOGGLE_STATUS_BAR, StrId::STR_HOME_SHORTCUT};
  // The orientation icon is the toolbar's T, turned to the mode it names; the underline
  // rotates with it around the same 32px square.
  static constexpr fui::Rotation kRotations[CrossPointSettings::ORIENTATION_COUNT] = {
      fui::Rotation::None, fui::Rotation::CW90, fui::Rotation::R180, fui::Rotation::CCW90};
  static constexpr fui::Rect kUnderlines[CrossPointSettings::ORIENTATION_COUNT] = {
      {4, 29, 24, 3}, {0, 4, 3, 24}, {4, 0, 24, 3}, {29, 4, 3, 24}};

  const int orientation = orientationShown % CrossPointSettings::ORIENTATION_COUNT;
  const bool touchOn = SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF;
  const int16_t labelHeight = screen.target().lineHeight(theme.smallText.font);
  const fui::Rect row = screen.takeTop(static_cast<int16_t>(kQuickControlSize + theme.spaceSm + labelHeight));
  const fui::Paint ink = fui::Paint::solid(fui::Color::Black);

  for (int id = 0; id < kTileCount; ++id) {
    const uint8_t kind = tileKind(id);
    const bool available = tileAvailable(kind);
    fui::BitmapRef icon{};
    switch (kind) {
      case CrossPointSettings::PANEL_TILE_NIGHT_MODE:
        icon = fui::bitmapFromIcon(icon_moon_32);
        break;
      case CrossPointSettings::PANEL_TILE_REFRESH:
        icon = fui::bitmapFromIcon(icon_refresh_cw_32);
        break;
      case CrossPointSettings::PANEL_TILE_ORIENTATION:
        icon = fui::bitmapFromIcon(icon_reader_text_24);
        break;
      case CrossPointSettings::PANEL_TILE_TOUCH:
        icon = fui::bitmapFromIcon(icon_pointer_32);
        break;
      case CrossPointSettings::PANEL_TILE_FOOTNOTES:
        icon = fui::bitmapFromIcon(icon_superscript_32);
        break;
      case CrossPointSettings::PANEL_TILE_BOOKMARK:
        icon = fui::bitmapFromIcon(icon_bookmark_32);
        break;
      case CrossPointSettings::PANEL_TILE_DICTIONARY:
        icon = fui::bitmapFromIcon(icon_book_a_32);
        break;
      case CrossPointSettings::PANEL_TILE_SYNC:
        icon = fui::bitmapFromIcon(icon_cloud_32);
        break;
      case CrossPointSettings::PANEL_TILE_READER_MENU:
        icon = fui::bitmapFromIcon(icon_menu_32);
        break;
      case CrossPointSettings::PANEL_TILE_STATUS_BAR:
        icon = fui::bitmapFromIcon(icon_panel_bottom_32);
        break;
      case CrossPointSettings::PANEL_TILE_HOME:
        icon = fui::bitmapFromIcon(icon_house_32);
        break;
      default:
        break;
    }
    const bool toggle =
        kind == CrossPointSettings::PANEL_TILE_NIGHT_MODE || kind == CrossPointSettings::PANEL_TILE_TOUCH;
    const bool enabled = kind == CrossPointSettings::PANEL_TILE_NIGHT_MODE ? SETTINGS.screenInverted != 0
                         : kind == CrossPointSettings::PANEL_TILE_TOUCH    ? touchOn
                                                                           : false;
    const char* label = toggle ? I18N.get(enabled ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF)
                        : kind == CrossPointSettings::PANEL_TILE_ORIENTATION ? I18N.get(kOrientNames[orientation])
                        : kind < CrossPointSettings::PANEL_TILE_COUNT        ? I18N.get(kTileNames[kind])
                                                                             : "";

    const int16_t left = static_cast<int16_t>(row.x + row.width * id / kTileCount);
    const int16_t right = static_cast<int16_t>(row.x + row.width * (id + 1) / kTileCount);
    const fui::Rect slot{left, row.y, static_cast<int16_t>(right - left), row.height};
    const fui::Rect circle{static_cast<int16_t>(slot.x + (slot.width - kQuickControlSize) / 2), row.y,
                           kQuickControlSize, kQuickControlSize};
    // A toggle shows its state at rest: filled when on, outlined when off. The other tiles
    // are one-shot actions and only outline while pressed.
    const int16_t value = static_cast<int16_t>(id);
    if (toggle && enabled) {
      screen.target().fill(circle, ink, kQuickControlSize / 2);
    } else if (toggle || fui::hasState(screen.frame().stateFor(ACTION_TILE, value), fui::StateActive)) {
      screen.target().stroke(circle, ink, 2, kQuickControlSize / 2);
    }
    const fui::Rect iconRect{static_cast<int16_t>(circle.x + (circle.width - kQuickIconSize) / 2),
                             static_cast<int16_t>(circle.y + (circle.height - kQuickIconSize) / 2), kQuickIconSize,
                             kQuickIconSize};
    const bool isOrientation = kind == CrossPointSettings::PANEL_TILE_ORIENTATION;
    screen.target().bitmap(iconRect, icon, fui::BitmapMode::Center,
                           fui::Paint::solid(enabled ? fui::Color::White : fui::Color::Black),
                           isOrientation ? kRotations[orientation] : fui::Rotation::None);
    if (!available) dimToHalf(iconRect.x, iconRect.y, iconRect.width, iconRect.height);
    if (isOrientation) {
      const auto& line = kUnderlines[orientation];
      screen.target().fill(fui::Rect{static_cast<int16_t>(iconRect.x + line.x),
                                     static_cast<int16_t>(iconRect.y + line.y), line.width, line.height},
                           ink);
    }
    // One line, ellipsised to the slot: four names share the sheet's width.
    fui::TextStyle text = theme.smallText;
    text.align = fui::TextAlign::Center;
    text.maxLines = 1;
    const fui::Rect labelRect{slot.x, static_cast<int16_t>(row.y + kQuickControlSize + theme.spaceSm), slot.width,
                              labelHeight};
    screen.target().text(labelRect, label, text);
    if (!available) dimToHalf(labelRect.x, labelRect.y, labelRect.width, labelRect.height);
    if (available) screen.frame().hit(slot, ACTION_TILE, value, fui::InputTouch);
  }
}

void FrontlightPanelActivity::render(RenderLock&&) {
  panelBottom = computePanelBottom();

  // fui::sheet draws the card body, its bottom rule, and the grabber during
  // renderUi(); nothing is hand-drawn around it any more.
  renderUi();

  // A tile that rewrote the whole frame (night mode) re-drives every pixel once; ordinary
  // repaints stay on the fast path. HALF is the mode that actually does that: it seeds the OLD
  // plane with the complement of the target, so no pixel idles in the no-transition cell. FULL
  // seeds white and skips every white-target pixel, which is the worst case here -- a polarity
  // flip is exactly when the outgoing frame's shape must not be left behind as stale charge.
  renderer.displayBuffer(cleanRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  cleanRefreshPending = false;
}
