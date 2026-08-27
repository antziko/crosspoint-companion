#include "MappedInputManager.h"

#include <GfxRenderer.h>
#include <HalFrontlight.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

// Global renderer (defined in main.cpp). Read the ACTUAL on-screen orientation
// so the front Left/Right swap follows what the user sees -- this is correct
// whether a screen rotates via SETTINGS.orientation (reader) or
// SETTINGS.displayOrientation (non-reader screens). APP_STATE.activeOrientation
// only tracks the reader's orientation, so it is the wrong source here.
extern GfxRenderer renderer;

namespace {
// True when the screen is currently rendered in LandscapeCW, where the per-
// orientation front map and side-button swap apply.
bool isCw() { return renderer.getOrientation() == GfxRenderer::Orientation::LandscapeClockwise; }

// Front Left/Right swap when the user opted in AND the screen is currently
// rendered in an orientation whose horizontal axis is flipped vs portrait
// (PortraitInverted / LandscapeCounterClockwise). LandscapeCW is intentionally NOT
// in this set, so the CW front-button override map is used as-is in CW without an
// additional Left/Right swap on top.
bool shouldSwapFrontButtons() {
  if (!SETTINGS.frontButtonFollowOrientation) {
    return false;
  }
  const auto o = renderer.getOrientation();
  return o == GfxRenderer::Orientation::PortraitInverted || o == GfxRenderer::Orientation::LandscapeCounterClockwise;
}

// Resolve the active front-button hardware index for a logical role, honoring the
// LandscapeCW override set when the screen is rendered in CW.
uint8_t frontBackHw() { return isCw() ? SETTINGS.frontButtonBackCW : SETTINGS.frontButtonBack; }
uint8_t frontConfirmHw() { return isCw() ? SETTINGS.frontButtonConfirmCW : SETTINGS.frontButtonConfirm; }
uint8_t frontLeftHw() { return isCw() ? SETTINGS.frontButtonLeftCW : SETTINGS.frontButtonLeft; }
uint8_t frontRightHw() { return isCw() ? SETTINGS.frontButtonRightCW : SETTINGS.frontButtonRight; }

// In CW with the side-swap setting on, the two physical side buttons trade roles.
bool swapSideButtons() { return isCw() && SETTINGS.swapSideButtonsCW; }
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const,
                                   const bool applySwap) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button (CW override in CW).
      return (gpio.*fn)(frontBackHw());
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button (CW override in CW).
      return (gpio.*fn)(frontConfirmHw());
    case Button::Left:
    case Button::Right: {
      // Logical Left/Right map to user-configured front buttons (CW override in
      // CW). When orient-front-buttons is on and the screen is rendered flipped,
      // swap the two so the physical button under the on-screen label performs
      // that label's action. applySwap=false lets a caller that does its own
      // orientation mapping (WordSelectNavigator) read the unswapped logical button.
      const bool swap = applySwap && shouldSwapFrontButtons();
      const bool wantLeft = (button == Button::Left);
      const uint8_t hw = (wantLeft != swap) ? frontLeftHw() : frontRightHw();
      return (gpio.*fn)(hw);
    }
    case Button::Up:
    case Button::Down:
      // Side buttons fixed for Up/Down, except the CW side-swap trades the two.
      return (gpio.*fn)(usesUpButton(button) ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      if (sideLayout == CrossPointSettings::SIDE_BUTTONS_DISABLED) {
        return false;
      }
      return (gpio.*fn)(usesUpButton(button) ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
    case Button::NavNext:
      // Logical "next item": side Down + front Right, with the control axis flipped in
      // INVERTED / LANDSCAPE_CCW (shouldSwapFrontButtons) so it matches the rotated hint labels.
      return shouldSwapFrontButtons()
                 ? (mapButton(Button::Up, fn, applySwap) || mapButton(Button::Left, fn, applySwap))
                 : (mapButton(Button::Down, fn, applySwap) || mapButton(Button::Right, fn, applySwap));
    case Button::NavPrevious:
      // Logical "previous item": side Up + front Left, axis-flipped in the same orientations.
      return shouldSwapFrontButtons()
                 ? (mapButton(Button::Down, fn, applySwap) || mapButton(Button::Right, fn, applySwap))
                 : (mapButton(Button::Up, fn, applySwap) || mapButton(Button::Left, fn, applySwap));
  }

  return false;
}

bool MappedInputManager::usesUpButton(const Button button) const {
  switch (button) {
    case Button::Up:
      return !swapSideButtons();
    case Button::Down:
      return swapSideButtons();
    case Button::PageBack:
    case Button::PageForward: {
      // PREV_NEXT: PageBack=UP, PageForward=DOWN. NEXT_PREV inverts that. The CW
      // side-swap (swapSideButtons) XOR-composes on top. Disabled side buttons map
      // to nothing -- report the lower (non-up) button rather than crashing callers.
      if (SETTINGS.sideButtonLayout == CrossPointSettings::SIDE_BUTTONS_DISABLED) {
        return false;
      }
      const bool wantPageBack = (button == Button::PageBack);
      const bool layoutInvert = (SETTINGS.sideButtonLayout == CrossPointSettings::NEXT_PREV);
      return (wantPageBack != layoutInvert) != swapSideButtons();
    }
    default:
      return false;
  }
}

namespace {
constexpr float LEFT_EDGE_BACK_GESTURE_FRAC_X = 0.25f;
constexpr float BOTTOM_EDGE_BACK_GESTURE_FRAC_Y = 0.14f;
constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
constexpr unsigned long TOUCH_HELD_OVERRIDE_WINDOW_MS = 250;
}  // namespace

// Reuses mapButton's logical->hardware dispatch so availability answers for exactly the
// button a press would read, remaps and orientation swaps included.
bool MappedInputManager::isAvailable(const Button button) const { return mapButton(button, &HalGPIO::hasButton, true); }

bool MappedInputManager::hasTouch() const { return gpio.hasTouch(); }

bool MappedInputManager::hasHomeKey() const { return gpio.hasHomeKey(); }

// Highest Button enum value, for the bitmask loops below.
constexpr uint8_t LAST_BUTTON = static_cast<uint8_t>(MappedInputManager::Button::NavPrevious);

void MappedInputManager::update() const {
  gpio.update();
  // A long press may fire only once per hold; clear the latch when the button is up.
  for (uint8_t value = 0; value <= LAST_BUTTON; ++value) {
    if (!isPressed(static_cast<Button>(value))) longPressFiredButtons &= ~(1u << value);
  }
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenLongPress(int& x, int& y) const {
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchLongPress(nx, ny)) return false;
  // Consuming the long-press implies acting on it: suppress the rest of the
  // contact so the finger lift can't also tap whatever the action opened.
  gpio.suppressTouchContact();
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  // Live contact position while the finger is down (no tap-slop gate) — drag tracking.
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer.tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::wasScreenTouchReleased() const { return gpio.wasTouchReleased(); }

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& theme = UITheme::getInstance().getTheme();
  const int rowStep = theme.getListRowStep(hasSubtitle);
  if (rowStep <= 0) return false;

  const int pageItems = theme.getListPageItems(listHeight, hasSubtitle);
  if (pageItems <= 0) return false;
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

MappedInputManager::RowTouch MappedInputManager::colTouch(int& col, const int left, const int colStep,
                                                          const int colCount, const int yStart, const int yEnd,
                                                          const int colWidth) const {
  if (colStep <= 0 || colCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (y < yStart || y >= yEnd || x < left) return false;
    const int c = (x - left) / colStep;
    if (c >= colCount) return false;
    if (colWidth > 0 && (x - left) % colStep >= colWidth) return false;
    col = c;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  float nxs = 0.0f;
  float nys = 0.0f;
  float nxe = 0.0f;
  float nye = 0.0f;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) return false;
  renderer.tapToLogical(nxs, nys, sx, sy);
  renderer.tapToLogical(nxe, nye, ex, ey);
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

bool MappedInputManager::wasBackGesture() const {
  // Back = left-to-right swipe starting near the left edge. Edge-anchored so that
  // mid-screen horizontal swipes stay available to activities that consume
  // SwipeDir::Left/Right (e.g. percent selection, image viewer).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = sx <= renderer.getScreenWidth() * LEFT_EDGE_BACK_GESTURE_FRAC_X && ex > sx &&
                   std::abs(ex - sx) > std::abs(ey - sy);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasMenuGesture() const {
  // Downward swipe starting at the top edge (mirror of the bottom-edge home gesture).
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int topEdgeBottom = static_cast<int>(renderer.getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y);
  const bool hit = sy <= topEdgeBottom && ey > sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasBottomEdgeUpSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const int bottomEdgeTop =
      renderer.getScreenHeight() - static_cast<int>(renderer.getScreenHeight() * BOTTOM_EDGE_BACK_GESTURE_FRAC_Y);
  const bool hit = sy >= bottomEdgeTop && ey < sy && std::abs(ey - sy) > std::abs(ex - sx);
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasReaderMenuSwipeUp() const { return gpio.hasHomeKey() && wasBottomEdgeUpSwipe(); }

bool MappedInputManager::wasHomeGesture() const {
  // Home-key boards (X4 Pro) use a short Home-key tap; their bottom-edge swipe
  // is free for the reader menu instead, so it cannot fire the same action twice.
  if (gpio.hasHomeKey()) return gpio.wasHomeKeyTapped();
  return wasBottomEdgeUpSwipe();
}

bool MappedInputManager::wasHomeKeyHold() const { return gpio.hasHomeKey() && gpio.wasHomeKeyLongPressed(); }

bool MappedInputManager::wasLightPanelGesture() const {
  // On lightless boards the same edge stays with the reader menu.
  return Frontlight.present() && wasMenuGesture();
}

#if FREEINK_CAP_TOUCH
bool MappedInputManager::wasPowerConfirmClick() const {
  if (!gpio.hasTouch() || SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM) return false;
  // Wait out the X4 Pro's frontlight double-click window before treating its
  // first release as Confirm; main.cpp supplies that one-frame event.
  if (BoardConfig::isX4Pro()) return powerConfirmClickFrame;
  return gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration();
}
#endif

bool MappedInputManager::wasPressed(const Button button, const bool applySwap) const {
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return mapButton(button, &HalGPIO::wasPressed, applySwap);
}

bool MappedInputManager::wasReleased(const Button button, const bool applySwap) const {
  if (button == Button::Back && wasBackGesture()) return true;
#if FREEINK_CAP_TOUCH
  if (button == Button::Confirm && wasPowerConfirmClick()) return true;
#endif
  return mapButton(button, &HalGPIO::wasReleased, applySwap);
}

bool MappedInputManager::wasLongPressed(const Button button, const unsigned long thresholdMs) const {
  if (!isPressed(button)) return false;
  const uint16_t bit = 1u << static_cast<uint8_t>(button);
  if ((longPressFiredButtons & bit) != 0 || getHeldTime() < thresholdMs) return false;
  longPressFiredButtons |= bit;
  suppressNextRelease(button);
  return true;
}

void MappedInputManager::suppressNextRelease(const Button button) const {
  suppressedReleaseButtons |= 1u << static_cast<uint8_t>(button);
}

bool MappedInputManager::consumeSuppressedRelease() const {
  if (suppressedReleaseButtons == 0) return false;
  uint16_t released = 0;
  for (uint8_t value = 0; value <= LAST_BUTTON; ++value) {
    const uint16_t bit = 1u << value;
    if ((suppressedReleaseButtons & bit) != 0 && mapButton(static_cast<Button>(value), &HalGPIO::wasReleased)) {
      released |= bit;
    }
  }
  suppressedReleaseButtons &= ~released;
  return released != 0;
}

bool MappedInputManager::isPressed(const Button button, const bool applySwap) const {
  return mapButton(button, &HalGPIO::isPressed, applySwap);
}

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const {
  if (!gpio.wasAnyPressed() && !gpio.wasAnyReleased() && touchHeldOverrideValid &&
      millis() - touchHeldOverrideAt <= TOUCH_HELD_OVERRIDE_WINDOW_MS) {
    return touchHeldOverrideMs;
  }
  touchHeldOverrideValid = false;
  return gpio.getHeldTime();
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the front-button action swap (see
  // shouldSwapFrontButtons / mapButton). Same source of truth -- the actual
  // render orientation -- so label and action always agree on every screen.
  const bool swapLabels = shouldSwapFrontButtons();
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles (CW override in CW) and return the
    // matching label. Same source as mapButton so label and action always agree.
    if (hw == frontBackHw()) {
      return back;
    }
    if (hw == frontConfirmHw()) {
      return confirm;
    }
    if (hw == frontLeftHw()) {
      return leftLabel;
    }
    if (hw == frontRightHw()) {
      return rightLabel;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
