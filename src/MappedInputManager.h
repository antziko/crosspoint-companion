#pragma once

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

  void update() const { gpio.update(); }
  // applySwap=false reads the raw logical button without the orient-front-buttons
  // Left/Right swap, for callers (e.g. WordSelectNavigator) that do their own
  // orientation mapping.
  bool wasPressed(Button button, bool applySwap = true) const;
  bool wasReleased(Button button, bool applySwap = true) const;
  bool isPressed(Button button, bool applySwap = true) const;
  bool hasTouch() const;
  bool wasScreenTapped(int& x, int& y) const;
  bool wasScreenTouchDown(int& x, int& y) const;
  bool isScreenTouchHeld(int& x, int& y) const;
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
  bool wasHomeGesture() const;
  bool wasMenuGesture() const;
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

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const, bool applySwap = true) const;
  bool wasBackGesture() const;
  // Fetch the pending swipe (if any) and map both endpoints to logical screen coords
  bool decodeSwipe(int& sx, int& sy, int& ex, int& ey) const;
  bool listItemFromPoint(int x, int y, int& index, int itemCount, int selectedIndex, int listTop, int listHeight,
                         bool hasSubtitle) const;
  void rememberTouchHeldTime() const;

  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
};
