#pragma once

#include <CrossPointSettings.h>
#include <CrossPointState.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;
constexpr unsigned long DICTIONARY_MESSAGE_DURATION_MS = 1500;

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

// Outcome of resolveSideNavAction — what a non-reader screen should do in
// response to its physical side Up/Down button this frame.
enum class SideNavAction { NONE, STEP, ROTATE };

// Side Up/Down double as list navigation AND (when the user has opted in via
// SETTINGS.sideLongPressButtonBehavior == ORIENTATION_CHANGE) a hold gesture
// that cycles the non-reader display orientation -- mirroring the reader's
// hold-to-rotate. A single press can't safely fire both, so when the gesture
// is enabled we switch to release-based detection (measure hold time first,
// like detectPageTurn's usePress branch); otherwise we keep the snappier
// press-based single-step navigation these screens already had.
inline SideNavAction resolveSideNavAction(const MappedInputManager& input, const MappedInputManager::Button button) {
  if (SETTINGS.sideLongPressButtonBehavior != SETTINGS.ORIENTATION_CHANGE) {
    return input.wasPressed(button) ? SideNavAction::STEP : SideNavAction::NONE;
  }

  if (!input.wasReleased(button)) return SideNavAction::NONE;
  return input.getHeldTime() > SKIP_HOLD_MS ? SideNavAction::ROTATE : SideNavAction::STEP;
}

// Cycles SETTINGS.displayOrientation by `step` (+1/-1, wrapping) and applies it
// immediately. Mirrors EpubReaderActivity's hold-to-rotate persistence.
inline void cycleDisplayOrientation(GfxRenderer& renderer, const int8_t step) {
  SETTINGS.displayOrientation =
      (SETTINGS.displayOrientation + step + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT;
  applyOrientation(renderer, SETTINGS.displayOrientation);
  SETTINGS.saveToFile();
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
  // True when the turn was triggered by a SIDE button (PageBack/PageForward)
  // rather than a FRONT button (Left/Right) / tilt / power. Lets the reader pick
  // a separate long-press behavior for side vs front buttons.
  bool fromSide;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  // Page turns fire on RELEASE when ANY long-press behavior is enabled (so the
  // hold can be measured first); otherwise on PRESS for snappier turns.
  const bool usePress =
      SETTINGS.longPressButtonBehavior == SETTINGS.OFF && SETTINGS.sideLongPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront =
      SETTINGS.frontButtonFollowOrientation && (APP_STATE.activeOrientation == CrossPointSettings::INVERTED ||
                                                APP_STATE.activeOrientation == CrossPointSettings::LANDSCAPE_CCW);
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;

  const bool sidePrev = usePress ? input.wasPressed(MappedInputManager::Button::PageBack)
                                 : input.wasReleased(MappedInputManager::Button::PageBack);
  const bool sideNext = usePress ? input.wasPressed(MappedInputManager::Button::PageForward)
                                 : input.wasReleased(MappedInputManager::Button::PageForward);
  const bool frontPrev = usePress ? input.wasPressed(prevButton) : input.wasReleased(prevButton);
  const bool frontNext = usePress ? input.wasPressed(nextButton) : input.wasReleased(nextButton);
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);

  const bool prev = tiltPrev || sidePrev || frontPrev;
  const bool next = tiltNext || sideNext || frontNext || powerTurn;
  const bool fromSide = sidePrev || sideNext;
  return {prev, next, tiltPrev || tiltNext, fromSide};
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils
