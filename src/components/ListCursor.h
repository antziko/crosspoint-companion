#pragma once

#include <HalGPIO.h>

// Whether a list should be drawing its navigation cursor yet.
//
// Every list starts its selection on row 0 so the buttons have somewhere to begin. On a
// TOUCH board that highlight states nothing the user chose -- it marks where the buttons
// would go, and the finger does not need it -- so it stays hidden until the first
// nav-button press (MappedInputManager::update), and entering an activity hides it again
// (Activity::onEnter). A tap deliberately reveals nothing: the File Browser does not
// re-enter its activity when it opens a folder, so a tap-revealed cursor would survive into
// every subfolder and draw row 0 highlighted there.
//
// A BUTTONS-ONLY board is the opposite case and is left exactly as it was: the cursor is
// the only way to act there, so hiding it would leave the user pressing Confirm with no
// idea what it would open. hide() is therefore a no-op on such a board.
//
// A list that opens positioned on a row OTHER than the first is left alone -- there the
// highlight carries information (the dictionary in use, the font in use), so suppression
// applies only while the selection still sits at the default row.
//
// One global, not per-activity state: the flag's whole lifetime is "since this screen was
// entered", and Activity::onEnter is the single place that begins one.
namespace ListCursor {

inline bool hiddenUntilNavigation = false;

// A no-op on a buttons-only board, where the cursor must always be visible.
inline void hide() { hiddenUntilNavigation = gpio.hasTouch(); }

// Returns true when this call actually revealed the cursor, so the caller can force the
// repaint that shows it. Needed because the press that reveals may not move anything — Up
// on the first row clamps to where it already is, and the screen then never repaints on its
// own, leaving the cursor invisible until some other input came along.
inline bool reveal() {
  const bool wasHidden = hiddenUntilNavigation;
  hiddenUntilNavigation = false;
  return wasHidden;
}

// True when the highlight for `selectedIndex` should not be drawn yet. Callers keep using
// the real selectedIndex for their paging maths -- only the highlight is withheld.
inline bool suppressed(const int selectedIndex) { return hiddenUntilNavigation && selectedIndex <= 0; }

}  // namespace ListCursor
