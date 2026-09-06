#pragma once
// Host-test stub for Arduino.h. WordSelectNavigator uses millis() for the word-step
// auto-repeat interval; the clock here is test-driven so a repeat can be stepped
// deterministically rather than slept through.

#include <cstdint>

namespace arduino_stub {
inline unsigned long& nowRef() {
  static unsigned long now = 0;
  return now;
}
}  // namespace arduino_stub

inline unsigned long millis() { return arduino_stub::nowRef(); }

// Test-only: move the fake clock. Not part of the real Arduino API.
inline void stubSetMillis(unsigned long ms) { arduino_stub::nowRef() = ms; }
inline void stubAdvanceMillis(unsigned long ms) { arduino_stub::nowRef() += ms; }
