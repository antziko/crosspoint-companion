#pragma once
#include <string>
#include <vector>

class GfxRenderer;
struct Rect;
struct ReadingTimeHistory;

// Shared Timeline/Heatmap presentation for the two reading-stats screens
// (ReadingStatsActivity = global, BookStatsActivity = per-book): row building
// from a ReadingTimeHistory, page-step scrolling, next-section jumps, the
// GitHub-style heatmap grid and the "no dated sessions" empty state. Owns the
// row list and scroll position; the owning activity keeps tab state, input
// mapping and its own contentRect().
class StatsTimelineView {
 public:
  // Rebuilds the row list from the history's weekly/monthly/yearly buckets
  // (most recent first). Leaves the list empty when there's no dated data.
  void build(const ReadingTimeHistory& history);

  bool empty() const { return rows.empty(); }

  // True when the list overflows `rect`, i.e. scrolling/jumping can move it.
  bool overflows(GfxRenderer& renderer, const Rect& rect) const;

  // Page-step scroll by one screenful. Return true when the offset changed
  // (caller redraws).
  bool pageUp(GfxRenderer& renderer, const Rect& rect);
  bool pageDown(GfxRenderer& renderer, const Rect& rect);

  // Jump to the next section header (Weekly -> Monthly -> Yearly), wrapping
  // back to the top once past the last one. Returns true when the offset moved.
  bool jumpToNextSection(GfxRenderer& renderer, const Rect& rect);

  // Draws the scrolled row list; falls back to renderEmptyState when empty.
  void renderList(GfxRenderer& renderer, const Rect& rect) const;

  // Stateless over `history` and the screen, so usable without a view instance
  // (the heatmap tab needs no row list). Draws the empty state itself when the
  // history has no dated data.
  static void renderHeatmap(GfxRenderer& renderer, const Rect& rect, const ReadingTimeHistory& history);
  static void renderEmptyState(GfxRenderer& renderer, const Rect& rect);

 private:
  struct Row {
    bool isSectionHeader;
    std::string label;
    std::string value;
  };

  std::vector<Row> rows;
  int scrollOffset = 0;

  // Rows that fit in `rect`; shared by paging, clamping and overflows() so
  // they can't disagree.
  int visibleRows(GfxRenderer& renderer, const Rect& rect) const;
  int maxOffset(GfxRenderer& renderer, const Rect& rect) const;
};
