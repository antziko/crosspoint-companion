#pragma once
#include <I18n.h>

#include <memory>

#include "GlobalReadingStats.h"
#include "StatsTimelineView.h"
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

  // ~1.7 KB (embeds two ReadingTimeHistory: local + remote snapshot) —
  // heap-allocated, never a stack local or by-value member (see
  // GlobalReadingStats.h / ReadingTimeHistory.h).
  std::unique_ptr<GlobalReadingStats> stats;
  // Cross-device display history: local overlaid with the synced remote snapshot,
  // built once in onEnter() (the heatmap renders every frame — don't re-fold per
  // frame). Equals the local history when nothing has been synced.
  std::unique_ptr<ReadingTimeHistory> displayHist;

  Tab selectedTab = Tab::Timeline;
  // Shared Timeline/Heatmap presentation (stacked Yearly/Monthly/Weekly + scroll).
  StatsTimelineView timeline;

  ButtonNavigator buttonNavigator;

  // Area below the tab bar shared by both tabs; single source of truth so loop()'s
  // scroll clamping and render()'s drawing always agree on available height.
  Rect contentRect() const;
};
