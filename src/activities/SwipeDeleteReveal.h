#pragma once

#include <BoardConfig.h>
#include <GfxRenderer.h>

#include <algorithm>

#include "ListTouchTarget.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "components/icons/listIcons.h"

// Swipe to delete for the hand-rolled lists (GUI.drawList + ListTouchTarget), the counterpart
// of UiAppHost::handleSwipeDelete on the FUI lists. A left swipe on a row reveals a trash
// button at the row's end; tapping it asks the screen to delete that row. Anything else while it
// shows -- a button, a right swipe, a hold, a tap elsewhere -- only hides it, and an up/down
// swipe hides it and still pages. Buttons hide it too because paging would move a different
// item under the saved rectangle.
//
// 24 bytes of plain members, no allocation. Written on the loop task under the render lock,
// read by draw() on the render task.
class SwipeDeleteReveal {
 public:
  enum class Kind : uint8_t { None, Consumed, Delete };
  struct Result {
    Kind kind = Kind::None;
    int row = -1;  // the row to delete, for Kind::Delete
  };

  // Loop task, ahead of the screen's own list input. allowed(ctx, row) says whether that row may
  // be revealed. Consumed and Delete both want a repaint; Consumed means the screen must not act
  // on this frame's input.
  Result handle(const MappedInputManager& input, const GfxRenderer& renderer, const ListTouchTarget& list,
                bool (*allowed)(void*, int), void* ctx) {
    Result result;
#if FREEINK_CAP_TOUCH
    if (!input.hasTouch()) return result;
    if (row_ >= 0) {
      // Swallow a button from press to release, so its hold cannot page the list meanwhile.
      if (input.wasAnyPressed()) buttonDown_ = true;
      if (buttonDown_) {
        if (input.wasAnyReleased()) {
          buttonDown_ = false;
          close();
        }
        result.kind = Kind::Consumed;
        return result;
      }
      int x = 0;
      int y = 0;
      if (input.wasScreenTapped(x, y)) {
        const bool onTrash = x >= button_.x && x < button_.x + button_.width && y >= button_.y &&
                             y < button_.y + button_.height;
        if (onTrash) {
          result.kind = Kind::Delete;
          result.row = row_;
        } else {
          result.kind = Kind::Consumed;
        }
        close();
        return result;
      }
      if (input.wasScreenLongPress(x, y)) {
        close();
        result.kind = Kind::Consumed;
        return result;
      }
    }

    int sx = 0;
    int sy = 0;
    const auto swipe = input.wasSwipe(&sx, &sy);
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      close();  // the screen pages and repaints
      return result;
    }
    if (swipe == MappedInputManager::SwipeDir::Right && row_ >= 0) {
      close();
      result.kind = Kind::Consumed;
      return result;
    }
    if (swipe != MappedInputManager::SwipeDir::Left) return result;
    Rect row{};
    const int index = list.indexAt(renderer, sx, sy, &row);
    if (index < 0 || !allowed(ctx, index)) return result;
    {
      RenderLock lock;
      const int width = std::min(row.height + kPad * 2, row.width / 2);
      button_ = Rect{row.x + row.width - width, row.y, width, row.height};
      row_ = index;
    }
    result.kind = Kind::Consumed;
#else
    (void)input;
    (void)renderer;
    (void)list;
    (void)allowed;
    (void)ctx;
#endif
    return result;
  }

  // Render task, right after GUI.drawList: a black button with a white trash at the row's end.
  void draw(const GfxRenderer& renderer) const {
#if FREEINK_CAP_TOUCH
    if (row_ < 0) return;
    renderer.fillRect(button_.x, button_.y, button_.width, button_.height, true);
    const freeink::Icon& icon = icon_trash_2_24;
    const int ox = button_.x + (button_.width - icon.w) / 2;
    const int oy = button_.y + (button_.height - icon.h) / 2;
    // SDK icon layout: row-major, MSB first, a clear bit is ink.
    const int rowBytes = (icon.w + 7) / 8;
    for (int r = 0; r < icon.h; ++r) {
      for (int c = 0; c < icon.w; ++c) {
        if (((icon.bits[r * rowBytes + (c >> 3)] >> (7 - (c & 7))) & 1) == 0) renderer.drawPixel(ox + c, oy + r, false);
      }
    }
#else
    (void)renderer;
#endif
  }

  // Hides the trash. True when it was showing.
  bool close() {
    if (row_ < 0) return false;
    RenderLock lock;
    row_ = -1;
    return true;
  }

 private:
  static constexpr int kPad = 8;
  int row_ = -1;
  Rect button_{};
  bool buttonDown_ = false;
};
