#pragma once

#include <HalGPIO.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward, NavNext, NavPrevious };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  void update() const { gpio.update(); }
  // applySwap=false reads the raw logical button without the orient-front-buttons
  // Left/Right swap, for callers (e.g. WordSelectNavigator) that do their own
  // orientation mapping.
  bool wasPressed(Button button, bool applySwap = true) const;
  bool wasReleased(Button button, bool applySwap = true) const;
  bool isPressed(Button button, bool applySwap = true) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
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

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const, bool applySwap = true) const;
};
