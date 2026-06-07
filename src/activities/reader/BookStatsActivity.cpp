#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SUMMARY_LINES = 2;

// Matches the abbreviations HalClock::formatDate() draws into its date strings —
// these are short calendar labels, not full sentences, so (like that code) they
// stay as plain English abbreviations rather than going through tr(STR_*).
constexpr const char* MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

const char* monthAbbr(uint8_t month) { return (month >= 1 && month <= 12) ? MONTH_ABBR[month - 1] : "?"; }
}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                                     std::string cachePath, int progressPercent)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(std::move(bookTitle)),
      cachePath(std::move(cachePath)),
      progressPercent(progressPercent) {}

void BookStatsActivity::onEnter() {
  Activity::onEnter();

  stats = BookReadingStats::load(cachePath);

  history = makeUniqueNoThrow<ReadingTimeHistory>();
  if (history) {
    ReadingTimeHistory::load(cachePath + "/book_time_history.bin", *history);
  }

  buildTimelineRows();
  requestUpdate();
}

void BookStatsActivity::onExit() { Activity::onExit(); }

void BookStatsActivity::buildTimelineRows() {
  timelineRows.clear();
  if (!history || !history->hasAnyData()) return;

  char label[24];
  char duration[32];

  bool sectionStarted = false;
  for (size_t i = 0; i < ReadingTimeHistory::WEEKLY_COUNT; ++i) {
    const auto& w = history->weekly[i];
    if (w.year == 0) continue;
    if (!sectionStarted) {
      timelineRows.push_back({true, tr(STR_STATS_WEEKLY), ""});
      sectionStarted = true;
    }
    snprintf(label, sizeof(label), "%s %u", monthAbbr(w.month), static_cast<unsigned>(w.day));
    BookReadingStats::formatDuration(w.seconds, duration, sizeof(duration));
    timelineRows.push_back({false, label, duration});
  }

  sectionStarted = false;
  for (size_t i = 0; i < ReadingTimeHistory::MONTHLY_COUNT; ++i) {
    const auto& m = history->monthly[i];
    if (m.year == 0) continue;
    if (!sectionStarted) {
      timelineRows.push_back({true, tr(STR_STATS_MONTHLY), ""});
      sectionStarted = true;
    }
    snprintf(label, sizeof(label), "%s %u", monthAbbr(m.month), static_cast<unsigned>(m.year));
    BookReadingStats::formatDuration(m.seconds, duration, sizeof(duration));
    timelineRows.push_back({false, label, duration});
  }

  sectionStarted = false;
  for (size_t i = 0; i < ReadingTimeHistory::YEARLY_COUNT; ++i) {
    const auto& y = history->yearly[i];
    if (y.year == 0) continue;
    if (!sectionStarted) {
      timelineRows.push_back({true, tr(STR_STATS_YEARLY), ""});
      sectionStarted = true;
    }
    snprintf(label, sizeof(label), "%u", static_cast<unsigned>(y.year));
    BookReadingStats::formatDuration(y.seconds, duration, sizeof(duration));
    timelineRows.push_back({false, label, duration});
  }
}

Rect BookStatsActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int summaryHeight = SUMMARY_LINES * (renderer.getLineHeight(SMALL_FONT_ID) + 2);

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + summaryHeight +
                  metrics.verticalSpacing + metrics.tabBarHeight + metrics.verticalSpacing;
  const int height = pageHeight - top - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, top, pageWidth, std::max(0, height)};
}

void BookStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (selectedTab != Tab::Timeline) {
      selectedTab = Tab::Timeline;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (selectedTab != Tab::Heatmap) {
      selectedTab = Tab::Heatmap;
      requestUpdate();
    }
    return;
  }

  if (selectedTab == Tab::Timeline && !timelineRows.empty()) {
    const Rect content = contentRect();
    const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + 10;
    const int visibleRows = std::max(1, content.height / rowHeight);
    const int maxOffset = std::max(0, static_cast<int>(timelineRows.size()) - visibleRows);

    buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] {
      scrollOffset = std::max(0, scrollOffset - 1);
      requestUpdate();
    });
    buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, maxOffset] {
      scrollOffset = std::min(maxOffset, scrollOffset + 1);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void BookStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOK_STATS),
                 bookTitle.c_str());

  const int leftX = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID) + 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  char totalBuf[32];
  BookReadingStats::formatDuration(stats.totalReadingSeconds, totalBuf, sizeof(totalBuf));
  std::string line1 = std::string(tr(STR_STATS_TIME_READING)) + ": " + totalBuf;
  const uint32_t datedSeconds = stats.totalReadingSeconds - stats.unattributedSeconds;
  if (datedSeconds > 0 && stats.unattributedSeconds > 0) {
    char datedBuf[32];
    char undatedBuf[32];
    BookReadingStats::formatDuration(datedSeconds, datedBuf, sizeof(datedBuf));
    BookReadingStats::formatDuration(stats.unattributedSeconds, undatedBuf, sizeof(undatedBuf));
    line1 += "  (" + std::string(tr(STR_STATS_DATED)) + " " + datedBuf + " / " + tr(STR_STATS_UNDATED) + " " +
             undatedBuf + ")";
  }
  renderer.drawText(SMALL_FONT_ID, leftX, y, line1.c_str());
  y += lineHeight;

  if (stats.totalReadingSeconds > 0 && progressPercent < 100) {
    std::string line2 = std::string(tr(STR_STATS_EST_REMAINING)) + ": ";
    if (progressPercent > 0) {
      const uint64_t remaining = (static_cast<uint64_t>(stats.totalReadingSeconds) *
                                  static_cast<uint64_t>(100 - progressPercent)) /
                                 static_cast<uint64_t>(progressPercent);
      char estBuf[32];
      BookReadingStats::formatDuration(
          static_cast<uint32_t>(std::min<uint64_t>(remaining, UINT32_MAX)), estBuf, sizeof(estBuf));
      line2 += estBuf;
    } else {
      line2 += tr(STR_STATS_CALCULATING);
    }
    renderer.drawText(SMALL_FONT_ID, leftX, y, line2.c_str());
  }
  y += lineHeight + metrics.verticalSpacing;

  const std::vector<TabInfo> tabs = {{tr(STR_STATS_TIMELINE), selectedTab == Tab::Timeline},
                                     {tr(STR_STATS_HEATMAP), selectedTab == Tab::Heatmap}};
  GUI.drawTabBar(renderer, Rect{0, y, pageWidth, metrics.tabBarHeight}, tabs, true);

  const Rect content = contentRect();
  if (selectedTab == Tab::Timeline) {
    renderTimeline(content);
  } else {
    renderHeatmap(content);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BookStatsActivity::renderTimeline(const Rect& rect) const {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (timelineRows.empty()) {
    const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
    const int midY = rect.y + rect.height / 2 - lineH;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_STATS_NO_HISTORY));
    if (!halClock.isAvailable()) {
      renderer.drawCenteredText(SMALL_FONT_ID, midY + lineH + 6, tr(STR_STATS_X3_ONLY));
    }
    return;
  }

  const int rowHeight = renderer.getLineHeight(UI_10_FONT_ID) + 10;
  int rowY = rect.y;
  for (size_t i = static_cast<size_t>(scrollOffset);
       i < timelineRows.size() && rowY + rowHeight <= rect.y + rect.height; ++i) {
    const auto& row = timelineRows[i];
    if (row.isSectionHeader) {
      renderer.drawText(UI_10_FONT_ID, rect.x + metrics.contentSidePadding, rowY + 5, row.label.c_str(), true,
                        EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_10_FONT_ID, rect.x + metrics.contentSidePadding + 14, rowY + 5, row.label.c_str());
      const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, row.value.c_str());
      renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - metrics.contentSidePadding - valueWidth, rowY + 5,
                        row.value.c_str());
    }
    rowY += rowHeight;
  }
}

void BookStatsActivity::renderHeatmap(const Rect& rect) const {
  if (!history || !history->hasAnyData()) {
    const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
    const int midY = rect.y + rect.height / 2 - lineH;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_STATS_NO_HISTORY));
    if (!halClock.isAvailable()) {
      renderer.drawCenteredText(SMALL_FONT_ID, midY + lineH + 6, tr(STR_STATS_X3_ONLY));
    }
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
  constexpr int CELL_GAP = 1;
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
  const uint32_t anchorDay = history->heatmapAnchorDay;
  const uint32_t anchorRow = (static_cast<uint32_t>(readingHistoryDayOfWeek(anchorDay)) + 5U) % 7U;
  const uint32_t anchorWeekMonday = anchorDay - anchorRow;
  const uint32_t oldestTrackedDay = anchorDay >= ReadingTimeHistory::HEATMAP_DAYS - 1
                                        ? anchorDay - (ReadingTimeHistory::HEATMAP_DAYS - 1)
                                        : 0;

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
      if (history->isHeatmapDaySet(anchorDay - dayIdx)) ++activeDays;
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

  // Grid: solid black for tracked days with at least one recorded session,
  // dithered gray for tracked days with none, and left blank for dates outside
  // the tracked range (the tail end of this week, or before recording began) —
  // so "no data yet" reads differently from "no reading that day".
  for (int col = 0; col < columns; ++col) {
    const uint32_t monday = weekMonday(col);
    for (uint32_t row = 0; row < static_cast<uint32_t>(ROWS); ++row) {
      const uint32_t dayIdx = monday + row;
      if (dayIdx > anchorDay || dayIdx < oldestTrackedDay) continue;
      const int cx = gridX + col * cellSize;
      const int cy = gridY + static_cast<int>(row) * cellSize;
      if (history->isHeatmapDaySet(anchorDay - dayIdx)) {
        renderer.fillRect(cx, cy, cellSize - CELL_GAP, cellSize - CELL_GAP, true);
      } else {
        renderer.fillRectDither(cx, cy, cellSize - CELL_GAP, cellSize - CELL_GAP, Color::LightGray);
      }
    }
  }
}
