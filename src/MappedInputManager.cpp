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
// Front Left/Right swap when the user opted in AND the screen is currently
// rendered in an orientation whose horizontal axis is flipped vs portrait.
bool shouldSwapFrontButtons() {
  if (!SETTINGS.frontButtonFollowOrientation) {
    return false;
  }
  const auto o = renderer.getOrientation();
  return o == GfxRenderer::Orientation::PortraitInverted ||
         o == GfxRenderer::Orientation::LandscapeCounterClockwise;
}
}  // namespace

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const,
                                   const bool applySwap) const {
  const auto sideLayout = SETTINGS.sideButtonLayout;

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
    case Button::Right: {
      // Logical Left/Right map to user-configured front buttons. When orient-
      // front-buttons is on and the screen is rendered flipped, swap the two so
      // the physical button under the on-screen label performs that label's
      // action. applySwap=false lets a caller that does its own orientation
      // mapping (WordSelectNavigator) read the unswapped logical button.
      const bool swap = applySwap && shouldSwapFrontButtons();
      const bool wantLeft = (button == Button::Left);
      const uint8_t hw = (wantLeft != swap) ? SETTINGS.frontButtonLeft : SETTINGS.frontButtonRight;
      return (gpio.*fn)(hw);
    }
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      switch (sideLayout) {
        case CrossPointSettings::PREV_NEXT:
          return (gpio.*fn)(HalGPIO::BTN_DOWN);
        case CrossPointSettings::NEXT_PREV:
          return (gpio.*fn)(HalGPIO::BTN_UP);
        case CrossPointSettings::SIDE_BUTTONS_DISABLED:
        default:
          return false;
      }
  }

  return false;
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
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return leftLabel;
    }
    if (hw == SETTINGS.frontButtonRight) {
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
