#pragma once
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
struct Rect;
struct ReadingTimeHistory;

// Shared Timeline/Heatmap presentation for the two reading-stats screens
// (ReadingStatsActivity = global, BookStatsActivity = per-book). The Timeline is
// one view stacking all three sections: Yearly (all years) and Monthly (the
// selected year's months) as 3-column grids, Weekly as a full-width list of the
// selected month's weeks.
//
// Navigation is a focus that moves down the levels (tab bar -> Yearly -> Monthly)
// with the side Up/Down buttons; Left/Right move the selection within the focused
// section. The selected year/month cell is drawn inverted (black fill, white
// text); the focused section additionally frames its selected cell (the cursor).
// Selecting a year re-points the Monthly grid; selecting a month re-points Weekly.
// The owning activity keeps tab state, input mapping and its own contentRect().
class StatsTimelineView {
 public:
  // Which section the cursor is in. None = focus is on the tab bar (activity
  // handles Left/Right there); the timeline draws no cursor frame.
  enum class Focus : uint8_t { None, Yearly, Monthly };

  // Initializes the selection to the latest recorded month (once), then builds.
  // Safe on an empty history (renders the empty state).
  void build(const ReadingTimeHistory& history);

  bool empty() const { return rows.empty(); }
  Focus focus() const { return focus_; }

  // Move the focus down (None -> Yearly -> Monthly) / up (Monthly -> Yearly ->
  // None). Return true when it moved. No rebuild — only the cursor frame changes.
  bool focusIn();
  bool focusOut();
  void resetFocus() { focus_ = Focus::None; }  // when the tab switches to Heatmap

  // Left/Right within the focused section: move the selected year or month
  // (wrapping). Selecting a year resets the month to that year's newest. No-op
  // when focus is None. Returns true when the selection moved (caller rescrolls).
  bool selectPrev(const ReadingTimeHistory& history);
  bool selectNext(const ReadingTimeHistory& history);

  // Scrolls so the focused selection (and, at the Monthly level, the Weekly rows
  // below it) stay visible. No-op when the list fits. Call after build()/select*.
  void scrollToSelection(GfxRenderer& renderer, const Rect& rect);

  // Draws the scrolled row list; falls back to renderEmptyState when empty.
  void renderList(GfxRenderer& renderer, const Rect& rect) const;

  // Heatmap tab + shared empty state, used directly by the activities.
  static void renderHeatmap(GfxRenderer& renderer, const Rect& rect, const ReadingTimeHistory& history);
  static void renderEmptyState(GfxRenderer& renderer, const Rect& rect);

 private:
  struct Row {
    enum class Kind : uint8_t { Header, Grid, Week };
    Kind kind = Kind::Header;
    // Header: title in text[0]. Grid: `count` label+value cells ("Jul 8'14") in
    // text[0..count-1]. Week: left label in text[0], right-aligned value in text[1].
    std::string text[3];
    uint8_t count = 0;          // Grid only: number of cells used (1..3)
    int8_t highlightCell = -1;  // Grid only: selected cell index (drawn inverted), -1 = none
    uint8_t gridSection = 0;    // Grid only: 1 = Yearly, 2 = Monthly (for cursor framing)
  };

  void initSelection(const ReadingTimeHistory& history);
  void rebuild(const ReadingTimeHistory& history);
  bool moveYear(const ReadingTimeHistory& history, int delta);
  bool moveMonth(const ReadingTimeHistory& history, int delta);

  std::vector<Row> rows;
  int scrollOffset = 0;

  Focus focus_ = Focus::None;  // opens on the tab bar; no year/month is selected until the user drills in
  uint16_t selYear_ = 0;
  uint8_t selMonth_ = 0;
  bool selectionInit = false;  // initSelection() runs once, on the first build()
  int selectedMonthRow_ = 0;   // row index of the selected Monthly row, for scroll-follow

  int visibleRows(GfxRenderer& renderer, const Rect& rect) const;
  int maxOffset(GfxRenderer& renderer, const Rect& rect) const;
};
