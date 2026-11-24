#include "StatsWeekPicker.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <utility>

#include "DailyReadingLog.h"
#include "ReadingTimeHistory.h"
#include "StatsTimelineView.h"
#include "components/UITheme.h"
#include "fontIds.h"

void StatsWeekPicker::init(std::string dailyLogPath, bool note, const ReadingTimeHistory& history) {
  path = std::move(dailyLogPath);
  deviceNote = note;
  focused_ = false;
  weeksBack = 0;
  endWeeksBack = 0;
  load(history);
}

void StatsWeekPicker::load(const ReadingTimeHistory& history) {
  const uint32_t back = static_cast<uint32_t>(weeksBack) * 7U;
  const uint32_t anchorMonday = StatsTimelineView::heatmapAnchorMonday(history);
  DailyReadingLog::readWeek(path.c_str(), anchorMonday >= back ? anchorMonday - back : 0, seconds);
}

bool StatsWeekPicker::focusIn() {
  if (focused_) return false;
  focused_ = true;
  return true;
}

bool StatsWeekPicker::focusOut() {
  if (!focused_) return false;
  focused_ = false;
  return true;
}

bool StatsWeekPicker::step(const ReadingTimeHistory& history, int delta) {
  if (!history.hasAnyData()) return false;
  const int next = std::clamp(weeksBack + delta, 0, StatsTimelineView::heatmapMaxWeeksBack(history));
  if (next == weeksBack) return false;
  weeksBack = next;
  load(history);
  return true;
}

bool StatsWeekPicker::selectAtPoint(GfxRenderer& renderer, const Rect& content, const ReadingTimeHistory& history,
                                    int x, int y) {
  Rect heat{}, chart{};
  split(renderer, content, heat, chart);
  const int back = StatsTimelineView::heatmapWeekAtPoint(renderer, heat, history, endWeeksBack, x, y);
  if (back < 0) return false;
  focused_ = true;
  if (back != weeksBack) {
    weeksBack = back;
    load(history);
  }
  return true;
}

void StatsWeekPicker::split(GfxRenderer& renderer, const Rect& content, Rect& heat, Rect& chart) const {
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int heatH = StatsTimelineView::heatmapCompactHeight(renderer);
  // Title, value labels, a usable bar height and two label lines.
  const int chartMin = lineH * 7;
  if (content.height - heatH < chartMin) {
    heat = content;
    chart = Rect{content.x, content.y + content.height, content.width, 0};
    return;
  }
  heat = Rect{content.x, content.y, content.width, heatH};
  chart = Rect{content.x, content.y + heatH, content.width, content.height - heatH};
}

void StatsWeekPicker::render(GfxRenderer& renderer, const Rect& content, const ReadingTimeHistory& history) const {
  if (!history.hasAnyData()) {
    StatsTimelineView::renderEmptyState(renderer, content);
    return;
  }
  Rect heat{}, chart{};
  split(renderer, content, heat, chart);
  const int columns = StatsTimelineView::heatmapColumns(renderer, heat);
  if (weeksBack < endWeeksBack) endWeeksBack = weeksBack;
  if (weeksBack > endWeeksBack + columns - 1) endWeeksBack = weeksBack - columns + 1;
  StatsTimelineView::renderHeatmap(renderer, heat, history, endWeeksBack, weeksBack, focused_);
  if (chart.height == 0) return;
  const uint32_t back = static_cast<uint32_t>(weeksBack) * 7U;
  const uint32_t anchorMonday = StatsTimelineView::heatmapAnchorMonday(history);
  StatsTimelineView::renderWeekChart(renderer, chart, history, anchorMonday >= back ? anchorMonday - back : 0, seconds,
                                     deviceNote);
}
