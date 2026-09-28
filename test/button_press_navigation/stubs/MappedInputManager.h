#pragma once

#include <cstdint>

extern unsigned long testNowMs;
inline unsigned long millis() { return testNowMs; }

class MappedInputManager {
 public:
  // Bit 0 = previous ({Up, Left}), bit 1 = next ({Down, Right}), matching ButtonNavigator's sets.
  enum class Button { Up, Down, Left = Up, Right = Down, NavPrevious = Up, NavNext = Down };
  struct Frame {
    uint32_t heldMs = 0;
    uint8_t pressed = 0;
    uint8_t released = 0;
    uint8_t held = 0;
  } frame;
  bool wasPressed(Button button) const { return frame.pressed & (1u << static_cast<unsigned>(button)); }
  bool wasReleased(Button button) const { return frame.released & (1u << static_cast<unsigned>(button)); }
  bool isPressed(Button button) const { return frame.held & (1u << static_cast<unsigned>(button)); }
  unsigned long getHeldTime() const { return frame.heldMs; }
};
