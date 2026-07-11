#include "MappedInputManager.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"

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

bool MappedInputManager::wasPressed(const Button button, const bool applySwap) const {
  return mapButton(button, &HalGPIO::wasPressed, applySwap);
}

bool MappedInputManager::wasReleased(const Button button, const bool applySwap) const {
  return mapButton(button, &HalGPIO::wasReleased, applySwap);
}

bool MappedInputManager::isPressed(const Button button, const bool applySwap) const {
  return mapButton(button, &HalGPIO::isPressed, applySwap);
}

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

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
