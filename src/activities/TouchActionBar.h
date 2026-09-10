#pragma once

#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"

// Bottom-anchored on-screen buttons for the prompt screens.
//
// BaseTheme::drawButtonHints early-returns on touch boards, so a screen whose only
// controls are the front-button hint strip shows its text and an empty strip — and the
// X4 Pro wires no front buttons at all, which leaves such a screen with no way to
// answer. These screens draw their own buttons instead, in the arrangement
// ConfirmationActivity and the FUI dialogs already use: equal boxes spread across the
// content width, the primary one filled.
//
// Inert on button boards: layout() with a false `enabled` leaves height() at 0 and
// hitAt() returning -1, so callers need no board conditionals of their own beyond the
// one they pass in.
class TouchActionBar {
 public:
  static constexpr int kMaxButtons = 3;

  // Reserve the strip. `enabled` is the caller's touch check; fontId sizes the boxes to
  // one line of that font. Call from onEnter(), before the text wrap budget is worked
  // out, so height() can be subtracted from it.
  void layout(const GfxRenderer& renderer, const bool enabled, const int fontId, const int count) {
    count_ = (enabled && count > 0) ? std::min(count, kMaxButtons) : 0;
    if (count_ == 0) {
      height_ = 0;
      return;
    }
    height_ = renderer.getLineHeight(fontId) + kPaddingY * 2;
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int screenWidth = renderer.getScreenWidth();
    const int inset = std::max(kMargin, metrics.contentSidePadding);
    const int gaps = kGap * (count_ - 1);
    const int width = std::max(1, (screenWidth - inset * 2 - gaps) / count_);
    const int y = renderer.getScreenHeight() - kMargin - height_;
    for (int i = 0; i < count_; i++) {
      buttons_[i] = {inset + i * (width + kGap), y, width, height_};
    }
    // The last box absorbs the rounding remainder so the row ends flush with the inset.
    buttons_[count_ - 1].w = screenWidth - inset - buttons_[count_ - 1].x;
  }

  // Total vertical space the bar occupies, including its bottom margin. 0 when inert.
  int reservedHeight() const { return count_ > 0 ? height_ + kMargin : 0; }
  bool active() const { return count_ > 0; }

  // `labels` must hold at least the count passed to layout(). primaryIndex draws that
  // one filled with inverted text (-1 for none).
  void draw(const GfxRenderer& renderer, const int fontId, const char* const* labels,
            const int primaryIndex = -1) const {
    const int radius = UITheme::getInstance().getMetrics().controlRadius;
    for (int i = 0; i < count_; i++) {
      const Button& b = buttons_[i];
      const bool primary = i == primaryIndex;
      if (primary) {
        if (radius > 0) {
          renderer.fillRoundedRect(b.x, b.y, b.w, b.h, radius, Color::Black);
        } else {
          renderer.fillRect(b.x, b.y, b.w, b.h, true);
        }
      } else if (radius > 0) {
        // White ground under the outline, not just the outline. The bar is drawn over
        // whatever the screen already holds, and on the sleep-image review that is a
        // full-bleed photo -- a hairline box and black text on a dark or busy picture is
        // unreadable. A no-op on the prompt screens, which are white there already.
        renderer.fillRoundedRect(b.x, b.y, b.w, b.h, radius, Color::White);
        renderer.drawRoundedRect(b.x, b.y, b.w, b.h, 1, radius, true);
      } else {
        renderer.fillRect(b.x, b.y, b.w, b.h, false);
        renderer.drawRect(b.x, b.y, b.w, b.h);
      }
      const char* label = labels[i] ? labels[i] : "";
      const int textWidth = renderer.getTextWidth(fontId, label);
      renderer.drawText(fontId, b.x + (b.w - textWidth) / 2, b.y + kPaddingY, label, !primary);
    }
  }

  // Index of the button containing (x, y), or -1.
  int hitAt(const int x, const int y) const {
    for (int i = 0; i < count_; i++) {
      const Button& b = buttons_[i];
      if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
    }
    return -1;
  }

 private:
  struct Button {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
  };

  static constexpr int kMargin = 20;
  static constexpr int kGap = 20;
  // Finger-sized without being tall: one line of text plus this above and below.
  static constexpr int kPaddingY = 12;

  Button buttons_[kMaxButtons];
  int count_ = 0;
  int height_ = 0;
};
