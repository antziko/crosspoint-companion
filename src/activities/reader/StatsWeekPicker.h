#pragma once
#include <cstdint>
#include <string>

class GfxRenderer;
struct Rect;
struct ReadingTimeHistory;

// Heatmap tab with a selected week and its Mon..Sun chart below, shared by the global and
// per-book stats screens. Down focuses the grid, Left/Right then step a week (Left = older),
// Up returns to the tab bar. A tap on a heatmap column selects that week; a swipe over the
// tab steps one week. The bars come from a DailyReadingLog file.
class StatsWeekPicker {
 public:
  // `dailyLogPath`: the DailyReadingLog file. `deviceNote`: label the bars as this device's
  // time, for a heatmap that also counts other devices.
  void init(std::string dailyLogPath, bool deviceNote, const ReadingTimeHistory& history);

  bool focused() const { return focused_; }
  bool focusIn();
  bool focusOut();
  void resetFocus() { focused_ = false; }

  // delta > 0 = older weeks. Returns true when the selection moved.
  bool step(const ReadingTimeHistory& history, int delta);
  // Tap on a heatmap column: select its week and focus the grid. Returns true when consumed.
  bool selectAtPoint(GfxRenderer& renderer, const Rect& content, const ReadingTimeHistory& history, int x, int y);

  void render(GfxRenderer& renderer, const Rect& content, const ReadingTimeHistory& history) const;

 private:
  void load(const ReadingTimeHistory& history);
  // Heatmap and chart bands of `content`; chart is empty when there is no room for it.
  void split(GfxRenderer& renderer, const Rect& content, Rect& heat, Rect& chart) const;

  std::string path;
  bool deviceNote = false;
  bool focused_ = false;
  int weeksBack = 0;
  // Weeks back shown in the rightmost column, kept so the selection stays on screen. The
  // render sets it, so the tap test reads the columns the user saw.
  mutable int endWeeksBack = 0;
  uint16_t seconds[7] = {};
};
