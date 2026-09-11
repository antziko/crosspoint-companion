#pragma once

#include <GfxRenderer.h>

#include "RenderLock.h"
#include "components/UITheme.h"

// Touch acknowledgment for list rows: tint the row the finger landed on and push just
// that row to the panel, so a tap is answered before whatever it opens appears.
//
// washRectDither is the primitive because it only ADDS ink -- the row's own text and icon
// survive the tint, where fillRect/fillRectDither would wipe the glyphs. The refresh is
// windowed to the row's rect; a full-frame refresh per tap would cost far more than the
// feedback is worth. (displayWindowRegion degrades to a full FAST refresh on X3, but every
// X3 profile is NO_TOUCH, so that path is unreachable from a touch handler.)
//
// The tint is deliberately never restored: a row that navigates is painted over by the new
// screen, and a row that stays put is redrawn by that screen's own next repaint. Restoring
// it would cost a second refresh for no information.
//
// Called from the loop task, so it takes the render lock. That lock is NOT recursive --
// never call this from render() or from inside a RenderLock scope.
inline void flashTouchedRow(const GfxRenderer& renderer, const Rect& row) {
  if (row.width <= 0 || row.height <= 0) return;
  RenderLock lock;
  renderer.washRectDither(row.x, row.y, row.width, row.height, Color::LightGray);
  renderer.displayWindowRegion(row.x, row.y, row.width, row.height);
}
