#pragma once

#include <FreeInkUI.h>

#include "MappedInputManager.h"
#include "components/ListCursor.h"

// One swipe of list paging, wrapping at both ends: a swipe past the last page returns to
// the first and one past the first jumps to the last. Button paging already wraps
// (ButtonNavigator::nextPageIndex), so without this a touch-only board is the one input
// method that dead-ends at the list edges.
//
// `forward` is a swipe UP (content moves up, the viewport walks down the list). Returns
// true when the viewport moved and the caller should repaint; false on a list that fits
// on one page, where there is nothing to scroll or to wrap to.
//
// Paging by nav.pageRows() (the rows the last build actually laid out), not the
// fixed-height estimate: rows that wrap or carry a subtitle are taller, and paging by the
// estimate skips the rows in between.
//
// A backward step is sized by the page being left, but the rows above may be taller, so
// the new viewport can stop short of the old top. `pinEnd` (optional) receives the row
// index the new viewport must reach (exclusive): the old top, or count after wrapping to
// the end. -1 when no pin is needed. The caller advances top after layout (see
// UiListActivity::renderListFrame); that only drops rows above, which the next swipe shows.
inline bool listSwipeScroll(freeink::ui::ListNav& nav, const bool forward, const int count, int* pinEnd = nullptr) {
  if (pinEnd) *pinEnd = -1;
  const int page = nav.pageRows();
  const int oldTop = nav.top;
  if (nav.scrollBy(forward ? page : -page, count)) {
    if (!forward && pinEnd) *pinEnd = oldTop;
    return true;
  }
  // scrollBy refused: the viewport is already parked at that end of the list.
  if (forward) {
    if (nav.top <= 0) return false;  // top AND bottom at once -- the list fits
    nav.top = 0;
    return true;
  }
  // scrollBy clamps to the last page as measured so far, and returns false when there is
  // no last page to go to.
  if (!nav.scrollBy(count, count)) return false;
  if (pinEnd) *pinEnd = count;
  return true;
}

// The same swipe for a list that moves its selection a page at a time (ButtonNavigator's
// next/previousPageIndex) instead of keeping a ListNav viewport: +1 for a swipe up, -1 for a
// swipe down, 0 for anything else. The caller pages the selection, which wraps at both ends;
// the highlight is withheld until the next nav-button press, as the swipe only scrolled.
inline int listSwipeStep(const MappedInputManager& input) {
  const auto swipe = input.wasSwipe();
  const int step = swipe == MappedInputManager::SwipeDir::Up ? 1 : swipe == MappedInputManager::SwipeDir::Down ? -1 : 0;
  if (step != 0) ListCursor::hideAfterSwipe();
  return step;
}
