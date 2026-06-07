#pragma once
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "GlobalReadingStats.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Home-screen global reading stats: total time read across all books (with a
// dated/undated split on mixed-device libraries) plus a Timeline/Heatmap tab
// pair backed by .crosspoint/global_time_history.bin (X3-only, dated sessions).
class ReadingStatsActivity final : public Activity {
 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

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

  // ~800 bytes (embeds a ReadingTimeHistory) — heap-allocated, never a stack
  // local or by-value member (see GlobalReadingStats.h / ReadingTimeHistory.h).
  std::unique_ptr<GlobalReadingStats> stats;

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
