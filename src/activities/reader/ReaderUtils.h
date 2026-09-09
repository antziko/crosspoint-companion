#pragma once

#include <CrossPointSettings.h>
#include <CrossPointState.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <components/bars/tap-zones.h>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long GO_BACK_OR_HOME_MS = GO_HOME_MS;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;
constexpr unsigned long DICTIONARY_MESSAGE_DURATION_MS = 1500;
// Inline flashcard-review tally toast. Longer than the others: it reports a result the
// user may want to actually read, not just an acknowledgement of their own action.
constexpr unsigned long INLINE_REVIEW_MESSAGE_DURATION_MS = 2500;

enum ReaderTouchAction : freeink::ui::ActionId {
  READER_TOUCH_PREV = 1,
  READER_TOUCH_NEXT = 3,
};

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
  // The orient-front-buttons Left/Right swap is handled centrally in
  // MappedInputManager::mapButton, so read the logical buttons plainly here.
  const auto prevButton = MappedInputManager::Button::Left;
  const auto nextButton = MappedInputManager::Button::Right;

  // On the release path, also fire the moment the hold crosses SKIP_HOLD_MS
  // instead of waiting for the button to come up: the chapter skip is what that
  // hold is for, and holding with no feedback until release reads as a dead
  // button. wasLongPressed() swallows the release it pre-empts, so the page
  // does not also turn on the way up.
  const auto triggered = [&](const MappedInputManager::Button button) {
    if (usePress) return input.wasPressed(button);
    return input.wasLongPressed(button, SKIP_HOLD_MS) || input.wasReleased(button);
  };
  const bool sidePrev = triggered(MappedInputManager::Button::PageBack);
  const bool sideNext = triggered(MappedInputManager::Button::PageForward);
  const bool frontPrev = triggered(prevButton);
  const bool frontNext = triggered(nextButton);
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);

  const bool prev = tiltPrev || sidePrev || frontPrev;
  const bool next = tiltNext || sideNext || frontNext || powerTurn;
  const bool fromSide = sidePrev || sideNext;
  return {prev, next, tiltPrev || tiltNext, fromSide};
}

struct TouchPageTurn {
  bool prev;
  bool next;
  unsigned long heldMs;
};

inline TouchPageTurn detectTouchPageTurn(GfxRenderer& renderer, const MappedInputManager& input) {
  TouchPageTurn result{false, false, 0};
  if (!SETTINGS.touchReaderControls || !input.hasTouch()) {
    return result;
  }

  if (SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_SWIPE) {
    // Horizontal swipes turn pages; taps stay free for the middle-third
    // reader-menu zone. A slow swipe never becomes a long-press chapter skip.
    const auto dir = input.wasSwipe();
    if (dir == MappedInputManager::SwipeDir::Left) {
      result.next = true;
    } else if (dir == MappedInputManager::SwipeDir::Right) {
      result.prev = true;
    }
    return result;
  }

  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) {
    return result;
  }

  const int16_t width = static_cast<int16_t>(renderer.getScreenWidth());
  const int16_t height = static_cast<int16_t>(renderer.getScreenHeight());
  // Outer thirds only: the middle third is the reader-menu tap
  // (isTouchMenuTap below), so it must not double as a page turn.
  const int16_t zoneWidth = width / 3;
  const bool inverted = SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_INVERTED_TAP;
  const freeink::ui::TapZone zones[] = {
      {freeink::ui::Rect{0, 0, zoneWidth, height}, inverted ? READER_TOUCH_NEXT : READER_TOUCH_PREV},
      {freeink::ui::Rect{static_cast<int16_t>(width - zoneWidth), 0, zoneWidth, height},
       inverted ? READER_TOUCH_PREV : READER_TOUCH_NEXT},
  };

  for (const auto& zone : zones) {
    if (!zone.enabled || !zone.rect.contains(static_cast<int16_t>(x), static_cast<int16_t>(y))) continue;
    result.prev = zone.action == READER_TOUCH_PREV;
    result.next = zone.action == READER_TOUCH_NEXT;
    break;
  }
  result.heldMs = gpio.lastTouchHeldMs();
  return result;
}

// A left-edge left-to-right swipe is the Back gesture (MappedInputManager::
// wasBackGesture) AND, in swipe page-turn mode, the previous-page swipe --
// and wasReleased(Button::Back) reports the gesture, so without this the
// gesture would close the book instead of paging back. On the reading surface
// the page turn wins; the physical Back button and the bottom-edge up-swipe
// still exit. handleBackNavigation() drops the gesture unconditionally, so
// only the Epub reader (which does not use it) needs this.
inline bool backGestureIsPageTurn(const MappedInputManager& input) {
  return SETTINGS.touchReaderControls == CrossPointSettings::TOUCH_READER_SWIPE && input.wasBackGesture();
}

// The reader-menu gesture actually in force on this board. Swipe Up is only
// offered where the Home key frees the bottom edge; settings.json is
// board-independent here, so a file written on an X4 Pro can carry SWIPE_UP
// onto a board where that swipe is already Home. Resolving it back to Tap
// keeps the menu reachable instead of silently stranding it.
inline uint8_t resolveShowReaderMenu(const MappedInputManager& input) {
  const uint8_t mode = SETTINGS.showReaderMenu;
  if (mode == CrossPointSettings::READER_MENU_SWIPE_UP && !input.hasHomeKey()) {
    return CrossPointSettings::READER_MENU_TAP;
  }
  return mode;
}

// Tap in the middle third of the screen: the tap path into the reader menu on
// every touch board. The page-turn tap zones are the outer thirds, so the
// middle is free in tap mode. The Off/Swipe Up alternatives are only surfaced
// on home-key boards (SettingsList), where the menu stays reachable through
// the key's hold.
inline bool isTouchMenuTap(const GfxRenderer& renderer, const MappedInputManager& input) {
  if (!input.hasTouch()) return false;
  if (resolveShowReaderMenu(input) != CrossPointSettings::READER_MENU_TAP) return false;
  int x = 0;
  int y = 0;
  if (!input.wasScreenTapped(x, y)) return false;
  const int width = renderer.getScreenWidth();
  // Same boundary math as detectTouchPageTurn's outer zones, so the middle
  // band meets them with no dead column when width % 3 != 0.
  const int zoneWidth = width / 3;
  return x >= zoneWidth && x < width - zoneWidth;
}

// Reader menu opens on the menu edge-swipe or a middle-third tap. On home-key
// boards a long press of the capacitive key runs the user-selected long-press
// function instead (SETTINGS.longPressMenuFunction), not the menu.
// Menu gestures honor showReaderMenu independently of touchReaderControls,
// which only gates page-turn touch zones in detectTouchPageTurn().
inline bool isTouchMenuGesture(const GfxRenderer& renderer, const MappedInputManager& input) {
  // A Home-key hold is board input, not a touch-reader control. On a frontlight board the
  // top-edge swipe belongs to the light panel, which makes this the reliable way in.
  if (input.wasHomeKeyHold()) return true;
  if (!input.hasTouch()) return false;
  if (input.wasMenuGesture()) return true;
  // Bottom-edge up-swipe variant. wasReaderMenuSwipeUp() is already false
  // without a Home key, so this cannot steal the Home gesture.
  if (resolveShowReaderMenu(input) == CrossPointSettings::READER_MENU_SWIPE_UP && input.wasReaderMenuSwipeUp()) {
    return true;
  }
  return isTouchMenuTap(renderer, input);
}

// One helper, blocking or deferred: the async form starts the refresh and
// returns so the caller can overlap CPU work with the panel's refresh time.
// Async callers must not touch the framebuffer until
// renderer.waitRefreshComplete() and must rebuild the differential baseline
// before the next page turn (the tiled grayscale cleanup does).
inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, bool async = false) {
  // "Never": getRefreshFrequency() returns the DISABLED sentinel, parking the
  // counter negative. Guard it so a negative counter is never read as "due".
  const bool disabled = (pagesUntilFullRefresh == CrossPointSettings::REFRESH_COUNTDOWN_DISABLED);
  const bool maintenanceDue = !disabled && pagesUntilFullRefresh <= 1;

  if (maintenanceDue) {
    // X3 no-flash maintenance: instead of the full-screen HALF flash, fire the OEM
    // AA-pre-BW(mid) differential waveform — changed pixels get the strong drive,
    // unchanged black/white pixels a gentle same-polarity top-up, settling ghosting
    // during the page turn itself. FAST_REFRESH is the driver's fallback when a clean
    // differential base isn't available; it degrades to fast + settle, never to a HALF
    // flash. Forced ghost-scrubs (image residue, popup/list wipe, initial paint) arrive
    // via the FORCE_FULL sentinel and deliberately take the HALF path — a gentle
    // reinforce can't clear that residue.
    const bool useBwReinforcement = renderer.isX3() &&
                                    pagesUntilFullRefresh != CrossPointSettings::REFRESH_COUNTDOWN_FORCE_FULL &&
                                    SETTINGS.refreshAction == CrossPointSettings::REFRESH_ACTION_BW_REINFORCEMENT;
    // Diagnostic (enable "SD Card Logging"): one line per maintenance page so the
    // no-flash path can be confirmed untethered on X3. reinforce=1 => AA-pre-BW(mid)
    // ran; reinforce=0 => HALF flash (check x3/action/countdown to see why).
    SdDebugLog::log("RFRSH", "maint x3=%d action=%d countdown=%d reinforce=%d", (int)renderer.isX3(),
                    (int)SETTINGS.refreshAction, pagesUntilFullRefresh, (int)useBwReinforcement);
    if (useBwReinforcement) {
      // Synchronous by design: displayGrayscaleBase has no async form, and the periodic
      // scrub was already blocking. `async` only applies to plain fast page turns.
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    if (async) {
      renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    if (!disabled) pagesUntilFullRefresh--;
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

struct BackNavCallback {
  void* ctx;
  void (*fn)(void*);
};

// Returns true if the back button was consumed (caller should return).
// Long press (>= GO_BACK_OR_HOME_MS):
// - default: go to file browser
// - with backShortToFileBrowser: go home
// Short press (< GO_BACK_OR_HOME_MS):
// - default: go home
// - with backShortToFileBrowser: go to file browser.
inline bool handleBackNavigation(const MappedInputManager& mappedInput, ActivityManager& activityManager,
                                 const char* filePath, BackNavCallback goHome) {
  // The reading surface deliberately has no swipe-to-exit path on any touch
  // board: the bottom-edge up-swipe already exits, and in swipe page-turn
  // mode a right swipe must page back instead. Back swipes stay available in menus and other activities; only
  // this reader-surface handler ignores them. Physical Back buttons are
  // unaffected: isPressed() is button-only, and this guard skips just the
  // gesture's own release frame.
  if (mappedInput.wasBackGesture()) {
    return false;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_BACK_OR_HOME_MS) {
    if (SETTINGS.backShortToFileBrowser) {
      goHome.fn(goHome.ctx);
    } else {
      activityManager.goToFileBrowser(filePath);
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < GO_BACK_OR_HOME_MS) {
    if (SETTINGS.backShortToFileBrowser) {
      activityManager.goToFileBrowser(filePath);
    } else {
      goHome.fn(goHome.ctx);
    }
    return true;
  }
  return false;
}

}  // namespace ReaderUtils
