#pragma once
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>

#include "BookReadingStats.h"
#include "ReadingTimeHistory.h"
#include "StatsTimelineView.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Per-book reading stats screen, opened from the reader menu. Shows total time
// read (with a dated/undated split on mixed-device libraries), an estimate of
// remaining reading time, and a Timeline/Heatmap tab pair backed by the book's
// dated history (cachePath/book_time_history.bin, X3-only).
class BookStatsActivity final : public Activity {
 public:
  // Snapshot of the in-progress reading session, passed at construction so
  // the heatmap can show today's reading without waiting for onExit().
  struct SessionContext {
    uint32_t elapsedSecs = 0;
    uint32_t thresholdSecs = 0;  // effective gate; 0 = always show
    bool dated = false;
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t dayOfWeek = 0;
  };

  explicit BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                             std::string cachePath, int progressPercent, SessionContext session);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Tab { Timeline, Heatmap };

  std::string bookTitle;
  std::string cachePath;
  int progressPercent = 0;
  SessionContext session;

  BookReadingStats stats;
  // ~785 bytes — heap-allocated, never a stack local (see ReadingTimeHistory.h).
  std::unique_ptr<ReadingTimeHistory> history;

  Tab selectedTab = Tab::Timeline;
  // Shared Timeline/Heatmap presentation (rows, scrolling, section jumps).
  StatsTimelineView timeline;

  ButtonNavigator buttonNavigator;

  // Area below the tab bar shared by both tabs; single source of truth so loop()'s
  // scroll clamping and render()'s drawing always agree on available height.
  Rect contentRect() const;
  void renderHeatmapTab(const Rect& rect) const;
};
