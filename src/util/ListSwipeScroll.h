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
inline bool listSwipeScroll(freeink::ui::ListNav& nav, const bool forward, const int count) {
  const int page = nav.pageRows();
  if (nav.scrollBy(forward ? page : -page, count)) return true;
  // scrollBy refused: the viewport is already parked at that end of the list.
  if (forward) {
    if (nav.top <= 0) return false;  // top AND bottom at once -- the list fits
    nav.top = 0;
    return true;
  }
  // scrollBy clamps to the real last page (it knows the measured page size), and returns
  // false when there is no last page to go to.
  return nav.scrollBy(count, count);
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
