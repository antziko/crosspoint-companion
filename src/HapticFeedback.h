#pragma once

// No board in this tree drives a vibration motor; kept so code shared with matcha-reader
// (PanelTouch.h) compiles unchanged.
namespace haptic_feedback {
inline void touchAction(bool /*longPress*/ = false) {}
}  // namespace haptic_feedback
