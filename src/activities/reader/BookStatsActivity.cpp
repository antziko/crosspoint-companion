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
    timeline.scrollToSelection(renderer, contentRect());
  }

  requestUpdate();
}

void BookStatsActivity::onExit() { Activity::onExit(); }

Rect BookStatsActivity::tabBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int summaryHeight = summaryLineCount() * (renderer.getLineHeight(SMALL_FONT_ID) + 2);
  const int y =
      metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + summaryHeight + metrics.verticalSpacing;
  return Rect{0, y, renderer.getScreenWidth(), metrics.tabBarHeight};
}

Rect BookStatsActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect tabs = tabBarRect();
  const int top = tabs.y + tabs.height + metrics.verticalSpacing;
  const int height = renderer.getScreenHeight() - top - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, top, renderer.getScreenWidth(), std::max(0, height)};
}

std::vector<TabInfo> BookStatsActivity::buildTabs() const {
  return {{tr(STR_STATS_TIMELINE), selectedTab == Tab::Timeline}, {tr(STR_STATS_HEATMAP), selectedTab == Tab::Heatmap}};
}

bool BookStatsActivity::handleTouch() {
  int tx = 0;
  int ty = 0;
  if (!mappedInput.wasScreenTapped(tx, ty)) return false;

  // Tab bar: a tap picks a tab outright instead of stepping towards it.
  int tab = 0;
  if (GUI.tabIndexFromPoint(renderer, tabBarRect(), buildTabs(), tx, ty, tab)) {
    const Tab next = (tab == 0) ? Tab::Timeline : Tab::Heatmap;
    if (next == selectedTab) return true;
    selectedTab = next;
    timeline.resetFocus();  // leaving Timeline: drop any year/month cursor
    requestUpdate();
    return true;
  }

  // Timeline: a tap on a year/month cell drills the focus into that section and
  // selects the cell, the touch counterpart of Down-then-Left/Right.
  if (selectedTab == Tab::Timeline && history) {
    const Rect content = contentRect();
    if (timeline.selectAtPoint(*history, renderer, content, tx, ty)) {
      timeline.scrollToSelection(renderer, content);
      requestUpdate();
      return true;
    }
  }
  return false;
}

void BookStatsActivity::loop() {
  if (mappedInput.hasTouch() && handleTouch()) return;

  // Left/Right: at the tab bar (focus None) switch Timeline/Heatmap; inside a
  // focused section move the year/month selection.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (timeline.focus() == StatsTimelineView::Focus::None) {
      if (selectedTab != Tab::Timeline) {
        selectedTab = Tab::Timeline;
        requestUpdate();
      }
    } else if (history && timeline.selectPrev(*history)) {
      timeline.scrollToSelection(renderer, contentRect());
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (timeline.focus() == StatsTimelineView::Focus::None) {
      if (selectedTab != Tab::Heatmap) {
        selectedTab = Tab::Heatmap;
        timeline.resetFocus();
        requestUpdate();
      }
    } else if (history && timeline.selectNext(*history)) {
      timeline.scrollToSelection(renderer, contentRect());
      requestUpdate();
    }
    return;
  }

  // Side Down/Up: move the focus down/up the levels (tab -> Yearly -> Monthly).
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] {
    if (selectedTab == Tab::Timeline && history && timeline.focusIn()) {
      timeline.scrollToSelection(renderer, contentRect());
      requestUpdate();
    }
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] {
    if (selectedTab == Tab::Timeline && timeline.focusOut()) {
      timeline.scrollToSelection(renderer, contentRect());
      requestUpdate();
    }
  });

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
  // Right-aligned on the same row: reading pace (avg real reading time per forward page,
  // sub-activity time excluded). formatDuration is too coarse here (anything <60s is "< 1 min"),
  // so render seconds/minutes directly with a "/pg" unit.
  if (session.pacePerPageSecs > 0) {
    char paceBuf[24];
    if (session.pacePerPageSecs < 60) {
      snprintf(paceBuf, sizeof(paceBuf), "%us/pg", session.pacePerPageSecs);
    } else {
      snprintf(paceBuf, sizeof(paceBuf), "%um %us/pg", session.pacePerPageSecs / 60, session.pacePerPageSecs % 60);
    }
    std::string paceLine = std::string(tr(STR_STATS_PAGE_PACE)) + ": " + paceBuf;
    const int paceWidth = renderer.getTextWidth(SMALL_FONT_ID, paceLine.c_str());
    renderer.drawText(SMALL_FONT_ID, rightEdge - paceWidth, y, paceLine.c_str());
  }
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

  GUI.drawTabBar(renderer, tabBarRect(), buildTabs(), true);

  const Rect content = contentRect();
  if (selectedTab == Tab::Timeline) {
    timeline.renderList(renderer, content);
  } else {
    renderHeatmapTab(content);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BookStatsActivity::renderHeatmapTab(const Rect& rect) const {
  // Committed on-disk history only, matching the Timeline tab: the current session is
  // folded in when the book is left (onExit -> commitReadingTime). Folding the live
  // session here would double-count the time the mid-session checkpoints have already
  // persisted into this same history (elapsedSecs is the whole session, not the
  // uncommitted remainder).
  if (!history) {
    StatsTimelineView::renderEmptyState(renderer, rect);
    return;
  }
  StatsTimelineView::renderHeatmap(renderer, rect, *history);
}
