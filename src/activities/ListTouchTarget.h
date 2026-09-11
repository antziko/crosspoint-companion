#pragma once

#include "TouchFeedback.h"
#include "components/UITheme.h"

class GfxRenderer;

// Touch selection for the hand-rolled list screens — the ones that paint with
// GUI.drawList() instead of inheriting UiListActivity's FUI list, and so never
// registered a hit rect with anything.
//
// render() records the exact arguments it passed to drawList(); loop() asks
// indexAt() which row a touch landed on. Recording rather than recomputing is
// the point: the content band's top and height are worked out inside each
// screen's render(), so a second copy in loop() would be free to drift away
// from the visuals the moment either is edited. The theme resolves the row
// geometry (BaseTheme::listIndexFromPoint and its overrides), so a screen picks
// up a theme's row metrics without knowing them.
//
// Trivially copyable, no allocation: 20 bytes of plain members.
class ListTouchTarget {
 public:
  // Call from render(), with the same values handed to GUI.drawList().
  void record(const Rect rect, const int itemCount, const int selectedIndex, const bool hasSubtitle = false) {
    rect_ = rect;
    itemCount_ = itemCount;
    selectedIndex_ = selectedIndex;
    hasSubtitle_ = hasSubtitle;
    valid_ = true;
  }

  // Forget the recorded list. For screens whose render() paints a list on some
  // frames and something else (a popup, an empty state) on others: without this
  // a tap on the popup would still hit the rows drawn underneath it.
  void clear() { valid_ = false; }

  // Item index at (x, y) in screen coordinates, or -1 when the point hits no row
  // (or nothing has been recorded yet, which is the state before the first paint).
  int indexAt(const GfxRenderer& renderer, const int x, const int y, Rect* rowRect = nullptr) const {
    if (!valid_) return -1;
    int index = -1;
    if (!GUI.listIndexFromPoint(renderer, rect_, itemCount_, selectedIndex_, hasSubtitle_, x, y, index, rowRect)) {
      return -1;
    }
    return index;
  }

  // Hit-test a tap AND acknowledge it: the row the finger landed on is tinted and pushed
  // to the panel before the caller acts on it, so a tap that opens something is answered
  // immediately instead of looking ignored until the next screen paints.
  //
  // Taps only. A hold that is polled frame by frame must keep using indexAt(), or it would
  // re-flash the row on every pass of the loop.
  int touchRow(const GfxRenderer& renderer, const int x, const int y) const {
    Rect row{};
    const int index = indexAt(renderer, x, y, &row);
    if (index < 0) return -1;
    // Deliberately does NOT reveal the navigation cursor (see ListCursor): the cursor marks
    // where the BUTTONS are, and a finger needs no such marker. Revealing it here left the
    // cursor on after a tap, so the File Browser drew row 0 highlighted in every folder the
    // user tapped into -- the activity is not re-entered on a folder change, so nothing
    // hid it again.
    flashTouchedRow(renderer, row);
    return index;
  }

 private:
  Rect rect_{};
  int itemCount_ = 0;
  int selectedIndex_ = -1;
  bool hasSubtitle_ = false;
  bool valid_ = false;
};
