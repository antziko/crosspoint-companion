#pragma once

#include <functional>
#include <initializer_list>

#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  // Borrowed lists are consumed synchronously and never retained.
  using Buttons = std::initializer_list<MappedInputManager::Button>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  // Buttons whose current hold started while THIS navigator was alive, one bit per
  // MappedInputManager::Button. Continuous navigation may only run for those -- see
  // onContinuous() for the hold-carried-in case this exists to stop.
  uint16_t continuousArmed_ = 0;
  static const MappedInputManager* mappedInput;

  [[nodiscard]] bool shouldNavigateContinuously() const;

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  [[nodiscard]] static Buttons getNextButtons() {
    static constexpr Buttons buttons = {MappedInputManager::Button::Down, MappedInputManager::Button::Right};
    return buttons;
  }
  [[nodiscard]] static Buttons getPreviousButtons() {
    static constexpr Buttons buttons = {MappedInputManager::Button::Up, MappedInputManager::Button::Left};
    return buttons;
  }
};
