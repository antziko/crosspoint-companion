#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SUMMARY_LINES = 3;
}  // namespace

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                                     std::string cachePath, int progressPercent, SessionContext session)
    : Activity("BookStats", renderer, mappedInput),
      bookTitle(std::move(bookTitle)),
      cachePath(std::move(cachePath)),
      progressPercent(progressPercent),
      session(session) {}

void BookStatsActivity::onEnter() {
  Activity::onEnter();

  stats = BookReadingStats::load(cachePath);

  history = makeUniqueNoThrow<ReadingTimeHistory>();
  if (history) {
    ReadingTimeHistory::load(cachePath + "/book_time_history.bin", *history);
    timeline.build(*history);
  }

  requestUpdate();
}

void BookStatsActivity::onExit() { Activity::onExit(); }

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

  if (selectedTab == Tab::Timeline && !timeline.empty()) {
    // Confirm: jump to the next section header (Weekly -> Monthly -> Yearly),
    // wrapping back to the top once past the last one.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (timeline.jumpToNextSection(renderer, contentRect())) {
        requestUpdate();
      }
      return;
    }

    buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] {
      if (timeline.pageUp(renderer, contentRect())) {
        requestUpdate();
      }
    });
    buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] {
      if (timeline.pageDown(renderer, contentRect())) {
        requestUpdate();
      }
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

  if (session.elapsedSecs > 0) {
    char sessionBuf[32];
    BookReadingStats::formatDuration(session.elapsedSecs, sessionBuf, sizeof(sessionBuf));
    std::string sessionLine = std::string(tr(STR_STATS_SESSION_TIME)) + ": " + sessionBuf;
    renderer.drawText(SMALL_FONT_ID, leftX, y, sessionLine.c_str());
  }
  y += lineHeight;

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
    timeline.renderList(renderer, content);
  } else {
    renderHeatmapTab(content);
  }

  // Only advertise Confirm when it can actually move the list — a list that fits
  // on one screen has nowhere to jump.
  const bool showSectionHint = selectedTab == Tab::Timeline && timeline.overflows(renderer, content);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), showSectionHint ? tr(STR_STATS_NEXT_SECTION) : "",
                                            tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BookStatsActivity::renderHeatmapTab(const Rect& rect) const {
  // If the current session qualifies (elapsed >= threshold, dated, valid date),
  // fold it into an in-memory scratch copy so today's reading is visible without
  // waiting for onExit() to commit to disk.
  std::unique_ptr<ReadingTimeHistory> scratchHistory;
  if (session.elapsedSecs > 0 && session.elapsedSecs >= session.thresholdSecs &&
      session.dated && session.year >= 2000) {
    scratchHistory = makeUniqueNoThrow<ReadingTimeHistory>();
    if (scratchHistory) {
      if (history) *scratchHistory = *history;
      scratchHistory->recordDay(session.year, session.month, session.day, session.dayOfWeek,
                                session.elapsedSecs);
    }
  }
  const ReadingTimeHistory* h = scratchHistory ? scratchHistory.get() : history.get();

  if (!h) {
    StatsTimelineView::renderEmptyState(renderer, rect);
    return;
  }
  StatsTimelineView::renderHeatmap(renderer, rect, *h);
}
