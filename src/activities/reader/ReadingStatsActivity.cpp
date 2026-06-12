#include "ReadingStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>

#include "BookReadingStats.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SUMMARY_LINES = 1;
}  // namespace

ReadingStatsActivity::ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReadingStats", renderer, mappedInput) {}

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();

  // One of the few non-reader screens that follows SETTINGS.displayOrientation
  // (the hold-to-rotate gesture is handled in loop(), see resolveSideNavAction).
  ReaderUtils::applyOrientation(renderer, SETTINGS.displayOrientation);

  stats = makeUniqueNoThrow<GlobalReadingStats>();
  if (stats) {
    GlobalReadingStats::load(*stats);
    timeline.build(stats->history);
  }

  requestUpdate();
}

void ReadingStatsActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

Rect ReadingStatsActivity::contentRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  // Second summary line only when a stats sync has brought in time from another
  // device (must match the render() condition or tabs and content drift apart).
  const int summaryLines = (stats && stats->remoteOtherSeconds > 0) ? SUMMARY_LINES + 1 : SUMMARY_LINES;
  const int summaryHeight = summaryLines * (renderer.getLineHeight(SMALL_FONT_ID) + 2);

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + summaryHeight +
                  metrics.verticalSpacing + metrics.tabBarHeight + metrics.verticalSpacing;
  const int height = pageHeight - top - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, top, pageWidth, std::max(0, height)};
}

void ReadingStatsActivity::loop() {
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

  // Scroll and section jumps only apply on the Timeline tab; the Heatmap tab
  // has nothing to scroll. The rotate gesture, however, must fire on BOTH tabs
  // -- so the resolveSideNavAction calls below stay outside this tab check.
  const bool canScrollTimeline = selectedTab == Tab::Timeline && !timeline.empty();

  // Confirm: jump to the next section header (Weekly -> Monthly -> Yearly),
  // wrapping back to the top once past the last one (Timeline only).
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (canScrollTimeline && timeline.jumpToNextSection(renderer, contentRect())) {
      requestUpdate();
    }
    return;
  }

  // Physical side Up/Down: page-step scroll (Timeline only) -- holding them
  // is reserved for the display-orientation-cycle gesture (both tabs).
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Up)) {
    case ReaderUtils::SideNavAction::STEP:
      if (canScrollTimeline && timeline.pageUp(renderer, contentRect())) {
        requestUpdate();
      }
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, 1);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Down)) {
    case ReaderUtils::SideNavAction::STEP:
      if (canScrollTimeline && timeline.pageDown(renderer, contentRect())) {
        requestUpdate();
      }
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, -1);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  const int leftX = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID) + 2;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const uint32_t totalSeconds = stats ? stats->totalReadingSeconds : 0;
  const uint32_t unattributedSeconds = stats ? stats->unattributedSeconds : 0;
  const uint32_t datedSeconds = totalSeconds - unattributedSeconds;

  char totalBuf[32];
  BookReadingStats::formatDuration(totalSeconds, totalBuf, sizeof(totalBuf));
  std::string line1 = std::string(tr(STR_STATS_TIME_READING)) + ": " + totalBuf;
  if (datedSeconds > 0 && unattributedSeconds > 0) {
    char datedBuf[32];
    char undatedBuf[32];
    BookReadingStats::formatDuration(datedSeconds, datedBuf, sizeof(datedBuf));
    BookReadingStats::formatDuration(unattributedSeconds, undatedBuf, sizeof(undatedBuf));
    line1 += "  (" + std::string(tr(STR_STATS_DATED)) + " " + datedBuf + " / " + tr(STR_STATS_UNDATED) + " " +
             undatedBuf + ")";
  }
  renderer.drawText(SMALL_FONT_ID, leftX, y, line1.c_str());
  y += lineHeight;

  // Cross-device total (local + last-synced remote counters from KOReader stats
  // sync). Condition must match contentRect()'s summary-line count.
  if (stats && stats->remoteOtherSeconds > 0) {
    char allBuf[32];
    BookReadingStats::formatDuration(stats->displayTotalSeconds(), allBuf, sizeof(allBuf));
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
  } else if (stats) {
    StatsTimelineView::renderHeatmap(renderer, content, stats->history);
  } else {
    StatsTimelineView::renderEmptyState(renderer, content);
  }

  // Only advertise Confirm when it can actually move the list — a list that fits
  // on one screen has nowhere to jump.
  const bool showSectionHint = selectedTab == Tab::Timeline && timeline.overflows(renderer, content);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), showSectionHint ? tr(STR_STATS_NEXT_SECTION) : "",
                                            tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
