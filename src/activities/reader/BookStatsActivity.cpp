#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

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
  const int summaryHeight = summaryLineCount() * (renderer.getLineHeight(SMALL_FONT_ID) + 2);

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
  const int rightEdge = pageWidth - metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID) + 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Line 1: "This session" (left) paired with "Est. left" (right-aligned).
  if (session.elapsedSecs > 0) {
    char sessionBuf[32];
    BookReadingStats::formatDuration(session.elapsedSecs, sessionBuf, sizeof(sessionBuf));
    std::string sessionLine = std::string(tr(STR_STATS_SESSION_TIME)) + ": " + sessionBuf;
    renderer.drawText(SMALL_FONT_ID, leftX, y, sessionLine.c_str());
  }
  if (stats.totalReadingSeconds > 0 && progressPercent < 100) {
    std::string estLine = std::string(tr(STR_STATS_EST_REMAINING)) + ": ";
    if (progressPercent > 0) {
      // Use the cross-device total: time spent on other devices counts toward how
      // long this book actually takes, so it sharpens the estimate.
      const uint64_t remaining =
          (static_cast<uint64_t>(stats.displayTotalSeconds()) * static_cast<uint64_t>(100 - progressPercent)) /
          static_cast<uint64_t>(progressPercent);
      char estBuf[32];
      BookReadingStats::formatDuration(static_cast<uint32_t>(std::min<uint64_t>(remaining, UINT32_MAX)), estBuf,
                                       sizeof(estBuf));
      estLine += estBuf;
    } else {
      estLine += tr(STR_STATS_CALCULATING);
    }
    const int estWidth = renderer.getTextWidth(SMALL_FONT_ID, estLine.c_str());
    renderer.drawText(SMALL_FONT_ID, rightEdge - estWidth, y, estLine.c_str());
  }
  y += lineHeight;

  // Line 2: total reading time on THIS device (dated + undated merged — clock
  // sources now date-stamp sessions on both X3 and X4, so the split is noise).
  char totalBuf[32];
  BookReadingStats::formatDuration(stats.totalReadingSeconds, totalBuf, sizeof(totalBuf));
  std::string readingLine = std::string(tr(STR_STATS_TIME_READING)) + ": " + totalBuf;
  renderer.drawText(SMALL_FONT_ID, leftX, y, readingLine.c_str());
  y += lineHeight;

  // Line 3: cross-device total (local + last-synced remote). Only shown once a
  // KOReader stats sync has actually brought in time from another device — on a
  // solo device it would just duplicate "Time reading".
  if (stats.remoteOtherSeconds > 0) {
    char allBuf[32];
    BookReadingStats::formatDuration(stats.displayTotalSeconds(), allBuf, sizeof(allBuf));
    char allLine[96];
    snprintf(allLine, sizeof(allLine), tr(STR_STATS_ALL_DEVICES_FORMAT), allBuf);
    renderer.drawText(SMALL_FONT_ID, leftX, y, allLine);
    y += lineHeight;
  }

  y += metrics.verticalSpacing;

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
  if (session.elapsedSecs > 0 && session.elapsedSecs >= session.thresholdSecs && session.dated &&
      session.year >= 2000) {
    scratchHistory = makeUniqueNoThrow<ReadingTimeHistory>();
    if (scratchHistory) {
      if (history) *scratchHistory = *history;
      scratchHistory->recordDay(session.year, session.month, session.day, session.dayOfWeek, session.elapsedSecs);
    }
  }
  const ReadingTimeHistory* h = scratchHistory ? scratchHistory.get() : history.get();

  if (!h) {
    StatsTimelineView::renderEmptyState(renderer, rect);
    return;
  }
  StatsTimelineView::renderHeatmap(renderer, rect, *h);
}
