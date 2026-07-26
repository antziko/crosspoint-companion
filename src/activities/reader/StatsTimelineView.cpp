#include "StatsTimelineView.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "ReadingTimeHistory.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Matches the abbreviations HalClock::formatDate() draws into its date strings —
// these are short calendar labels, not full sentences, so (like that code) they
// stay as plain English abbreviations rather than going through tr(STR_*).
constexpr const char* MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const char* monthAbbr(uint8_t month) { return (month >= 1 && month <= 12) ? MONTH_ABBR[month - 1] : "?"; }

int timelineRowHeight(GfxRenderer& renderer) { return renderer.getLineHeight(UI_10_FONT_ID) + 10; }

// Compact H'MM reading-duration for the timeline cells (e.g. 19'02, 99'15) —
// hours, an apostrophe, then zero-padded minutes, so a grid of them stays narrow.
// Distinct from BookReadingStats::formatDuration()'s "1h 44m" long form, which
// the totals/ETA lines keep.
void formatDurationCompact(uint32_t seconds, char* buf, size_t len) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  snprintf(buf, len, "%lu'%02lu", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
}

// Columns in the Yearly/Monthly grid sections.
constexpr int GRID_COLUMNS = 3;
}  // namespace

void StatsTimelineView::initSelection(const ReadingTimeHistory& history) {
  selYear_ = 0;
  selMonth_ = 0;
  // Latest recorded month (monthly[0] is newest); fall back to the latest year
  // when there's no monthly data at all.
  if (history.monthly[0].year != 0) {
    selYear_ = history.monthly[0].year;
    selMonth_ = history.monthly[0].month;
  } else if (history.yearly[0].year != 0) {
    selYear_ = history.yearly[0].year;
  }
}

void StatsTimelineView::build(const ReadingTimeHistory& history) {
  if (!selectionInit) {
    initSelection(history);
    selectionInit = true;
  }
  rebuild(history);
}

bool StatsTimelineView::focusIn() {
  if (focus_ == Focus::None) {
    focus_ = Focus::Yearly;
    return true;
  }
  if (focus_ == Focus::Yearly) {
    focus_ = Focus::Monthly;
    return true;
  }
  return false;  // already at the deepest focusable level (Monthly)
}

bool StatsTimelineView::focusOut() {
  if (focus_ == Focus::Monthly) {
    focus_ = Focus::Yearly;
    return true;
  }
  if (focus_ == Focus::Yearly) {
    focus_ = Focus::None;  // back up to the tab bar
    return true;
  }
  return false;
}

bool StatsTimelineView::selectNext(const ReadingTimeHistory& history) {
  if (focus_ == Focus::Yearly) return moveYear(history, +1);
  if (focus_ == Focus::Monthly) return moveMonth(history, +1);
  return false;
}

bool StatsTimelineView::selectPrev(const ReadingTimeHistory& history) {
  if (focus_ == Focus::Yearly) return moveYear(history, -1);
  if (focus_ == Focus::Monthly) return moveMonth(history, -1);
  return false;
}

bool StatsTimelineView::moveYear(const ReadingTimeHistory& history, int delta) {
  // All recorded years, newest-first (yearly[] is already ordered).
  uint16_t years[ReadingTimeHistory::YEARLY_COUNT];
  int n = 0;
  for (size_t i = 0; i < ReadingTimeHistory::YEARLY_COUNT; ++i)
    if (history.yearly[i].year != 0) years[n++] = history.yearly[i].year;
  if (n <= 1) return false;

  int pos = 0;
  for (int p = 0; p < n; ++p)
    if (years[p] == selYear_) {
      pos = p;
      break;
    }
  const int np = ((pos + delta) % n + n) % n;  // wrap
  if (np == pos) return false;
  selYear_ = years[np];

  // Re-point the month to the newest month of the newly-selected year (monthly[]
  // is newest-first, so the first match is the newest).
  selMonth_ = 0;
  for (size_t i = 0; i < ReadingTimeHistory::MONTHLY_COUNT; ++i)
    if (history.monthly[i].year == selYear_) {
      selMonth_ = history.monthly[i].month;
      break;
    }
  rebuild(history);
  return true;
}

bool StatsTimelineView::moveMonth(const ReadingTimeHistory& history, int delta) {
  // The selected year's months, newest-first.
  uint8_t months[ReadingTimeHistory::MONTHLY_COUNT];
  int n = 0;
  for (size_t i = 0; i < ReadingTimeHistory::MONTHLY_COUNT; ++i)
    if (history.monthly[i].year == selYear_) months[n++] = history.monthly[i].month;
  if (n <= 1) return false;

  int pos = 0;
  for (int p = 0; p < n; ++p)
    if (months[p] == selMonth_) {
      pos = p;
      break;
    }
  const int np = ((pos + delta) % n + n) % n;  // wrap
  if (np == pos) return false;
  selMonth_ = months[np];
  rebuild(history);
  return true;
}

void StatsTimelineView::rebuild(const ReadingTimeHistory& history) {
  rows.clear();
  scrollOffset = 0;
  selectedMonthRow_ = 0;
  if (!history.hasAnyData()) return;

  // Count for an exact reserve: all years, the selected year's months, and the
  // selected month's weeks. Yearly/Monthly pack GRID_COLUMNS cells per row.
  size_t yearlyEntries = 0, monthlyInSel = 0, weeklyInSel = 0;
  for (size_t i = 0; i < ReadingTimeHistory::YEARLY_COUNT; ++i)
    if (history.yearly[i].year != 0) ++yearlyEntries;
  for (size_t i = 0; i < ReadingTimeHistory::MONTHLY_COUNT; ++i)
    if (history.monthly[i].year == selYear_) ++monthlyInSel;
  for (size_t i = 0; i < ReadingTimeHistory::WEEKLY_COUNT; ++i)
    if (history.weekly[i].year == selYear_ && history.weekly[i].month == selMonth_) ++weeklyInSel;
  const size_t gridRows =
      (yearlyEntries + GRID_COLUMNS - 1) / GRID_COLUMNS + (monthlyInSel + GRID_COLUMNS - 1) / GRID_COLUMNS;
  rows.reserve(gridRows + weeklyInSel + 3);

  char cell[24];
  char dur[16];

  auto pushHeader = [&](const char* text) {
    Row header;
    header.kind = Row::Kind::Header;
    header.text[0] = text;
    rows.push_back(std::move(header));
  };

  // Grid accumulator shared by the Yearly and Monthly sections: buffers up to
  // GRID_COLUMNS dash-joined cells, then emits one row. `highlight` marks the
  // selected cell (drawn inverted); gridSectionTag tags the row's section so
  // renderList knows whether to frame it as the cursor.
  Row gridRow;
  int gridCol = 0;
  uint8_t gridSectionTag = 0;  // 1 = Yearly, 2 = Monthly
  auto flushGrid = [&]() {
    if (gridCol == 0) return;
    gridRow.kind = Row::Kind::Grid;
    gridRow.count = static_cast<uint8_t>(gridCol);
    rows.push_back(std::move(gridRow));
    gridRow = Row{};
    gridCol = 0;
  };
  auto pushGridCell = [&](const char* text, bool highlight) {
    gridRow.gridSection = gridSectionTag;
    if (highlight) gridRow.highlightCell = static_cast<int8_t>(gridCol);
    gridRow.text[gridCol] = text;
    ++gridCol;
    if (gridCol == GRID_COLUMNS) flushGrid();
  };

  // Yearly: all years, the selected year boxed. Label and value grouped ("2026
  // 99'15") so each cell reads as a unit; fixed-width labels keep them aligned.
  gridSectionTag = 1;
  bool sectionStarted = false;
  for (size_t i = 0; i < ReadingTimeHistory::YEARLY_COUNT; ++i) {
    const auto& y = history.yearly[i];
    if (y.year == 0) continue;
    if (!sectionStarted) {
      pushHeader(tr(STR_STATS_YEARLY));
      sectionStarted = true;
    }
    formatDurationCompact(y.seconds, dur, sizeof(dur));
    snprintf(cell, sizeof(cell), "%u  %s", static_cast<unsigned>(y.year), dur);
    pushGridCell(cell, y.year == selYear_);
  }
  flushGrid();

  // Monthly: only the selected year's months, the selected month boxed ("Jul 8'14").
  gridSectionTag = 2;
  sectionStarted = false;
  for (size_t i = 0; i < ReadingTimeHistory::MONTHLY_COUNT; ++i) {
    const auto& m = history.monthly[i];
    if (m.year != selYear_) continue;
    if (!sectionStarted) {
      pushHeader(tr(STR_STATS_MONTHLY));
      sectionStarted = true;
    }
    formatDurationCompact(m.seconds, dur, sizeof(dur));
    snprintf(cell, sizeof(cell), "%s  %s", monthAbbr(m.month), dur);
    const bool selected = m.month == selMonth_;
    // The in-progress grid row is inserted at rows.size() on its next flush, and
    // nothing else is pushed in between — so record that as the selected row now.
    if (selected) selectedMonthRow_ = static_cast<int>(rows.size());
    pushGridCell(cell, selected);
  }
  flushGrid();

  // Weekly: the selected month's weeks.
  sectionStarted = false;
  for (size_t i = 0; i < ReadingTimeHistory::WEEKLY_COUNT; ++i) {
    const auto& w = history.weekly[i];
    if (w.year != selYear_ || w.month != selMonth_) continue;
    if (!sectionStarted) {
      pushHeader(tr(STR_STATS_WEEKLY));
      sectionStarted = true;
    }
    Row row;
    row.kind = Row::Kind::Week;
    snprintf(cell, sizeof(cell), "%s %u", monthAbbr(w.month), static_cast<unsigned>(w.day));
    row.text[0] = cell;
    formatDurationCompact(w.seconds, dur, sizeof(dur));
    row.text[1] = dur;
    rows.push_back(std::move(row));
  }
}

int StatsTimelineView::visibleRows(GfxRenderer& renderer, const Rect& rect) const {
  return std::max(1, rect.height / timelineRowHeight(renderer));
}

int StatsTimelineView::maxOffset(GfxRenderer& renderer, const Rect& rect) const {
  return std::max(0, static_cast<int>(rows.size()) - visibleRows(renderer, rect));
}

void StatsTimelineView::scrollToSelection(GfxRenderer& renderer, const Rect& rect) {
  if (static_cast<int>(rows.size()) <= visibleRows(renderer, rect) || focus_ != Focus::Monthly) {
    scrollOffset = 0;  // fits, or focus is up top (Yearly/tab) — show from the top
    return;
  }
  // Monthly focus, overflow: keep the selected month (and the Weekly rows below
  // it) on screen by parking the selected row a line from the top.
  scrollOffset = std::max(0, std::min(selectedMonthRow_ - 1, maxOffset(renderer, rect)));
}

void StatsTimelineView::renderList(GfxRenderer& renderer, const Rect& rect) const {
  if (rows.empty()) {
    renderEmptyState(renderer, rect);
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = timelineRowHeight(renderer);
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int gridLeft = rect.x + metrics.contentSidePadding;
  const int gridRight = rect.x + rect.width - metrics.contentSidePadding;
  // Fixed GRID_COLUMNS so cells line up vertically even on a partially-filled row.
  const int colWidth = std::max(1, (gridRight - gridLeft) / GRID_COLUMNS);

  int rowY = rect.y;
  for (size_t i = static_cast<size_t>(scrollOffset); i < rows.size() && rowY + rowHeight <= rect.y + rect.height; ++i) {
    const auto& row = rows[i];
    switch (row.kind) {
      case Row::Kind::Header:
        renderer.drawText(UI_10_FONT_ID, gridLeft, rowY + 5, row.text[0].c_str(), true, EpdFontFamily::BOLD);
        break;
      case Row::Kind::Grid: {
        // Yearly/Monthly: up to GRID_COLUMNS grouped "label  value" cells. The
        // selection highlight matches the selected tab (Lyra drawTabBar): a
        // rounded rect at cornerRadius 6 with 8px horizontal padding. The FOCUSED
        // section's selected cell is filled + white text (the cursor, only one on
        // screen); a selected cell in a non-focused section gets the same shape as
        // an outline. CELL_GAP leaves space before the box within the column.
        constexpr int CELL_GAP = 6;       // gap before the box within the column
        constexpr int BOX_PAD_X = 8;      // horizontal padding inside the box (matches the tab)
        constexpr int CORNER_RADIUS = 6;  // matches the tab's cornerRadius
        const bool sectionFocused =
            (row.gridSection == 1 && focus_ == Focus::Yearly) || (row.gridSection == 2 && focus_ == Focus::Monthly);
        for (int c = 0; c < row.count; ++c) {
          const int boxX = gridLeft + c * colWidth + CELL_GAP;
          const int textX = boxX + BOX_PAD_X;
          const int textW = renderer.getTextWidth(UI_10_FONT_ID, row.text[c].c_str());
          const int boxY = rowY + 2;
          const int boxW = textW + 2 * BOX_PAD_X;
          const int boxH = lineHeight + 6;
          // Only surface the year/month selection once the user has drilled in from
          // the tab bar; on open (focus None) the timeline shows no boxed cell.
          const bool selected = focus_ != Focus::None && c == row.highlightCell;
          const bool cursor = selected && sectionFocused;
          if (cursor) {
            renderer.fillRoundedRect(boxX, boxY, boxW, boxH, CORNER_RADIUS, Color::Black);
          } else if (selected) {
            renderer.drawRoundedRect(boxX, boxY, boxW, boxH, 1, CORNER_RADIUS, true);
          }
          renderer.drawText(UI_10_FONT_ID, textX, rowY + 5, row.text[c].c_str(), !cursor);
        }
        break;
      }
      case Row::Kind::Week: {
        // Weekly: full-width entry, value right-aligned to the content edge.
        renderer.drawText(UI_10_FONT_ID, gridLeft + 14, rowY + 5, row.text[0].c_str());
        const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, row.text[1].c_str());
        renderer.drawText(UI_10_FONT_ID, gridRight - valueWidth, rowY + 5, row.text[1].c_str());
        break;
      }
    }
    rowY += rowHeight;
  }
}

void StatsTimelineView::renderEmptyState(GfxRenderer& renderer, const Rect& rect) {
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int midY = rect.y + rect.height / 2 - lineH;
  renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_STATS_NO_HISTORY));
  if (!halClock.isAvailable()) {
    renderer.drawCenteredText(SMALL_FONT_ID, midY + lineH + 6, tr(STR_STATS_NEEDS_CLOCK));
  }
}

void StatsTimelineView::renderHeatmap(GfxRenderer& renderer, const Rect& rect, const ReadingTimeHistory& history) {
  if (!history.hasAnyData()) {
    renderEmptyState(renderer, rect);
    return;
  }

  // 730 days laid out as a GitHub-style calendar grid: 7 rows (Mon..Sun, a fixed
  // weekday order so the day-of-week labels never rotate) x up to 106 columns
  // (ISO calendar weeks), oldest week on the left. Cells whose date falls outside
  // the tracked range (before the oldest recorded day, or after today) are left
  // blank, so "no data yet" reads differently from "no reading that day". A
  // summary line and month ticks sit above the grid; cell size adapts to the
  // remaining space with a floor — in portrait that floor clips the oldest
  // columns, landscape is wide enough to fit them all at the floor size or larger.
  constexpr int ROWS = 7;
  constexpr int MAX_COLUMNS = static_cast<int>((ReadingTimeHistory::HEATMAP_DAYS + 2 * ROWS - 1) / ROWS);
  constexpr int MIN_CELL_SIZE = 8;
  constexpr int MARGIN = 4;
  constexpr int CELL_GAP = 2;
  // Fixed Mon..Sun row order (ISO week) — the legend never rotates with "today",
  // unlike a most-recent-day-relative layout whose labels would cycle through the
  // week and stop matching whichever weekday lands in row 0.
  static constexpr const char* DAY_INITIAL[ROWS] = {"M", "T", "W", "T", "F", "S", "S"};

  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  // Each row must be at least one line-height tall, or the day-of-week initial
  // drawn in it overlaps its neighbours and the column turns into an illegible smear.
  const int cellFloor = std::max(MIN_CELL_SIZE, smallLineH);
  const int dayLabelW =
      std::max(renderer.getTextWidth(SMALL_FONT_ID, "M"), renderer.getTextWidth(SMALL_FONT_ID, "W")) + 4;
  const int headerH = smallLineH * 2 + 4;

  const int gridAreaX = rect.x + MARGIN + dayLabelW;
  const int gridAreaY = rect.y + MARGIN + headerH;
  const int gridAreaWidth = std::max(ROWS * cellFloor, rect.width - MARGIN * 2 - dayLabelW);
  const int gridAreaHeight = std::max(ROWS * cellFloor, rect.height - MARGIN * 2 - headerH);

  int cellSize = std::max(cellFloor, gridAreaWidth / MAX_COLUMNS);
  cellSize = std::min(cellSize, std::max(cellFloor, gridAreaHeight / ROWS));
  const int columns = std::min(MAX_COLUMNS, std::max(1, gridAreaWidth / cellSize));

  const int gridWidth = columns * cellSize;
  const int gridHeight = ROWS * cellSize;
  const int gridX = gridAreaX + std::max(0, (gridAreaWidth - gridWidth) / 2);
  const int gridY = gridAreaY + std::max(0, (gridAreaHeight - gridHeight) / 2);

  // Anchor week: the Monday on/before heatmapAnchorDay. HalClock's day-of-week
  // convention is 1=Sunday..7=Saturday (readingHistoryDayOfWeek matches it); the
  // number of days past that week's Monday is `(dow + 5) % 7`, which doubles as
  // the Mon=0..Sun=6 row index for any day index.
  const uint32_t anchorDay = history.heatmapAnchorDay;
  const uint32_t anchorRow = (static_cast<uint32_t>(readingHistoryDayOfWeek(anchorDay)) + 5U) % 7U;
  const uint32_t anchorWeekMonday = anchorDay - anchorRow;
  const uint32_t oldestTrackedDay =
      anchorDay >= ReadingTimeHistory::HEATMAP_DAYS - 1 ? anchorDay - (ReadingTimeHistory::HEATMAP_DAYS - 1) : 0;

  // Monday day-index of the week drawn in column `col` (0 = oldest/leftmost).
  const auto weekMonday = [&](int col) -> uint32_t {
    const uint32_t back = static_cast<uint32_t>(columns - 1 - col) * 7U;
    return anchorWeekMonday >= back ? anchorWeekMonday - back : 0;
  };

  // Summary line: the visible date range plus how many tracked days had any
  // reading, e.g. "Jun '24 - Jun '26  |  142 days active". The tally walks the
  // same in-range test the grid below uses, so the count matches what's drawn.
  size_t activeDays = 0;
  for (int col = 0; col < columns; ++col) {
    const uint32_t monday = weekMonday(col);
    for (uint32_t row = 0; row < static_cast<uint32_t>(ROWS); ++row) {
      const uint32_t dayIdx = monday + row;
      if (dayIdx > anchorDay || dayIdx < oldestTrackedDay) continue;
      if (history.isHeatmapDaySet(anchorDay - dayIdx)) ++activeDays;
    }
  }
  uint16_t oldYear, newYear;
  uint8_t oldMonth, oldDay, newMonth, newDay;
  readingHistoryDateFromDayIndex(weekMonday(0), oldYear, oldMonth, oldDay);
  readingHistoryDateFromDayIndex(anchorDay, newYear, newMonth, newDay);

  char summary[64];
  snprintf(summary, sizeof(summary), "%s '%02u - %s '%02u  |  %u %s", monthAbbr(oldMonth),
           static_cast<unsigned>(oldYear % 100), monthAbbr(newMonth), static_cast<unsigned>(newYear % 100),
           static_cast<unsigned>(activeDays), tr(STR_STATS_DAYS_ACTIVE));
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y + MARGIN, summary);

  // Month ticks along the top of the grid, drawn wherever a column's Monday
  // crosses into a new calendar month.
  int lastTickMonth = -1;
  for (int col = 0; col < columns; ++col) {
    uint16_t y;
    uint8_t m, d;
    readingHistoryDateFromDayIndex(weekMonday(col), y, m, d);
    if (m != lastTickMonth) {
      lastTickMonth = m;
      renderer.drawText(SMALL_FONT_ID, gridX + col * cellSize, gridY - smallLineH - 2, monthAbbr(m));
    }
  }

  // Day-of-week initials down the left edge — fixed Mon..Sun, one-to-one with
  // DAY_INITIAL and every column's row order.
  for (int row = 0; row < ROWS; ++row) {
    const char* initial = DAY_INITIAL[row];
    const int textW = renderer.getTextWidth(SMALL_FONT_ID, initial);
    const int labelY = gridY + row * cellSize + (cellSize - smallLineH) / 2;
    renderer.drawText(SMALL_FONT_ID, gridX - 4 - textW, labelY, initial);
  }

  // Grid: shade each tracked day by reading-intensity level — light gray for
  // <=30min, dark gray for <=1h, solid black for >1h. Each cell's footprint is
  // cellSize - CELL_GAP, leaving a CELL_GAP-px white strip on its right and bottom, so
  // every neighbour (horizontal and vertical) is separated by CELL_GAP px of white and
  // no two cells ever share or double a border. Untracked days and tracked days
  // with no reading both render as plain white (None), so "no data yet" and "no
  // reading that day" are visually indistinguishable by design.
  for (int col = 0; col < columns; ++col) {
    const uint32_t monday = weekMonday(col);
    for (uint32_t row = 0; row < static_cast<uint32_t>(ROWS); ++row) {
      const uint32_t dayIdx = monday + row;
      if (dayIdx > anchorDay || dayIdx < oldestTrackedDay) continue;
      const int cx = gridX + col * cellSize;
      const int cy = gridY + static_cast<int>(row) * cellSize;
      const int cellFootprint = cellSize - CELL_GAP;
      switch (history.getHeatmapLevel(anchorDay - dayIdx)) {
        case ReadingTimeHistory::HeatmapLevel::Heavy:
          renderer.fillRect(cx, cy, cellFootprint, cellFootprint, true);
          break;
        case ReadingTimeHistory::HeatmapLevel::Moderate:
          renderer.fillRectDither(cx, cy, cellFootprint, cellFootprint, Color::DarkGray);
          break;
        case ReadingTimeHistory::HeatmapLevel::Light:
          renderer.fillRectDither(cx, cy, cellFootprint, cellFootprint, Color::LightGray);
          break;
        case ReadingTimeHistory::HeatmapLevel::None:
          break;
      }
    }
  }

  // Single frame around the whole grid, 1px white gutter outside the cells so an
  // edge cell never merges into it. gridWidth/gridHeight include the trailing
  // right/bottom CELL_GAP, so subtract it before adding the gutter + frame.
  renderer.drawRect(gridX - 2, gridY - 2, gridWidth - CELL_GAP + 4, gridHeight - CELL_GAP + 4, true);
}
