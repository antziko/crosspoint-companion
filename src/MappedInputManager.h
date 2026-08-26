#pragma once

#include <BoardConfig.h>
#include <HalGPIO.h>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, const GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const;
  // applySwap=false reads the raw logical button without the orient-front-buttons
  // Left/Right swap, for callers (e.g. WordSelectNavigator) that do their own
  // orientation mapping.
  bool wasPressed(Button button, bool applySwap = true) const;
  bool wasReleased(Button button, bool applySwap = true) const;
  bool isPressed(Button button, bool applySwap = true) const;
  // Fires ONCE while the button is still down, as soon as it has been held for
  // thresholdMs, and marks the button's eventual release to be swallowed. Without
  // it a release-triggered action (page turn) would fire again on the way up.
  bool wasLongPressed(Button button, unsigned long thresholdMs) const;
  // True on the frame a suppressed release actually arrives; ActivityManager
  // consumes it ahead of activity input.
  bool consumeSuppressedRelease() const;
  bool hasTouch() const;
  // True on boards with a capacitive Home key (X4 Pro), which frees the bottom
  // screen edge for a gesture of its own.
  bool hasHomeKey() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  // One-shot long-press from the SDK touch classifier, fired WHILE the finger
  // is still down (stationary contact held past the SDK threshold). Consuming
  // it suppresses the remainder of the contact — its continued hold and its
  // release edge — so the ensuing finger lift can't also tap-dismiss the popup
  // the long-press opened. The SDK owns that latch and self-clears it once the
  // contact ends.
  bool wasScreenLongPress(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
  // Raw release edge, also true when the contact ended in a swipe or drag-off
  // (which wasScreenTapped never reports). InputSnapshot builders forward it
  // off-target so FreeInkUI routing clears its pressed-element state.
  bool wasScreenTouchReleased() const;
  bool wasTapInRect(int x, int y, int width, int height) const;
  bool wasListItemTapped(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  bool wasListItemTouchedDown(int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                              bool hasSubtitle) const;

  // Combined touch interaction for a band of equal rows with caller-supplied
  // geometry — the shared hit-test for lists the theme helpers above do not
  // cover (custom row heights, option prompts, menus). Down = a held
  // tap-candidate is on a row (update the selection highlight); Tap = a tap
  // released on one (activate). rowHeight limits the hit to the top rowHeight
  // px of each step (0 = the full step, no gap band).
  enum class RowTouch : uint8_t { None, Down, Tap };
  RowTouch rowTouch(int& row, int top, int rowStep, int rowCount, int xStart = 0, int xEnd = INT32_MAX,
                    int rowHeight = 0) const;
  // Horizontal variant for side-by-side button pairs (confirmation prompts).
  RowTouch colTouch(int& col, int left, int colStep, int colCount, int yStart, int yEnd, int colWidth = 0) const;

  SwipeDir wasSwipe() const;
  // Back = left-to-right swipe anchored at the left edge. Public so the reader
  // surface can exclude it from a plain SwipeDir::Right (see
  // ReaderUtils::handleBackNavigation).
  bool wasBackGesture() const;
  bool wasHomeGesture() const;
  // A Home-key hold, for surfaces that want a second action from the key.
  bool wasHomeKeyHold() const;
  bool wasMenuGesture() const;
  // Upward swipe starting at the bottom edge. On boards with no Home key this
  // IS the Home gesture; on home-key boards the edge is free and it can serve
  // as the reader-menu gesture instead.
  bool wasBottomEdgeUpSwipe() const;
  // The bottom-edge up-swipe as the reader-menu gesture (SHOW_READER_MENU's
  // Swipe Up option). Only meaningful on home-key boards; elsewhere the same
  // swipe is Home and this returns false.
  bool wasReaderMenuSwipeUp() const;
  // Top-edge down-swipe opens the light panel when the board has a frontlight.
  // ActivityManager consumes it before activity input.
  bool wasLightPanelGesture() const;
#if FREEINK_CAP_TOUCH
  // Power short-click acting as Confirm. On boards with no front buttons this
  // is the only Confirm there is, so wasPressed/wasReleased fold it in.
  bool wasPowerConfirmClick() const;
  // X4 Pro delays a single power click until its frontlight double-click window
  // expires. The main loop supplies that one-frame event here.
  void setPowerConfirmClickFrame(const bool clicked) { powerConfirmClickFrame = clicked; }
#endif
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  const GfxRenderer& getRenderer() const { return renderer; }
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // True when the given logical button currently resolves to the physical UP side
  // button (BTN_UP). Meaningful for Up / Down / PageBack / PageForward (it folds in
  // the Side Button Layout and the CW side-swap); false for any other button.
  // Callers that draw side-button clues by physical position use this to pick the
  // corner each label sits in. Returns false for PageBack/PageForward when the side
  // buttons are disabled.
  bool usesUpButton(Button button) const;

 private:
  HalGPIO& gpio;
  const GfxRenderer& renderer;
#if FREEINK_CAP_TOUCH
  // One-frame latch set by the main loop; see setPowerConfirmClickFrame().
  bool powerConfirmClickFrame = false;
#endif

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const, bool applySwap = true) const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  void rememberTouchHeldTime() const;
  void suppressNextRelease(Button button) const;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
  // One bit per Button. `fired` stops a long press repeating while still held and
  // is cleared on release; `suppressed` marks releases still owed a swallow.
  mutable uint16_t longPressFiredButtons = 0;
  mutable uint16_t suppressedReleaseButtons = 0;
};
