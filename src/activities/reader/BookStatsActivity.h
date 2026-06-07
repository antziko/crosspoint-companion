#pragma once
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "ReadingTimeHistory.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Per-book reading stats screen, opened from the reader menu. Shows total time
// read (with a dated/undated split on mixed-device libraries), an estimate of
// remaining reading time, and a Timeline/Heatmap tab pair backed by the book's
// dated history (cachePath/book_time_history.bin, X3-only).
class BookStatsActivity final : public Activity {
 public:
  explicit BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                             std::string cachePath, int progressPercent);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Tab { Timeline, Heatmap };

  struct TimelineRow {
    bool isSectionHeader;
    std::string label;
    std::string value;
  };

  std::string bookTitle;
  std::string cachePath;
  int progressPercent = 0;

  BookReadingStats stats;
  // ~785 bytes — heap-allocated, never a stack local (see ReadingTimeHistory.h).
  std::unique_ptr<ReadingTimeHistory> history;

  Tab selectedTab = Tab::Timeline;
  std::vector<TimelineRow> timelineRows;
  int scrollOffset = 0;

  ButtonNavigator buttonNavigator;

  void buildTimelineRows();
  // Area below the tab bar shared by both tabs; single source of truth so loop()'s
  // scroll clamping and render()'s drawing always agree on available height.
  Rect contentRect() const;
  void renderTimeline(const Rect& rect) const;
  void renderHeatmap(const Rect& rect) const;
};
