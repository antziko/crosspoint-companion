#pragma once
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "GlobalReadingStats.h"
#include "StatsTimelineView.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;
struct TabInfo;

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
  enum class Tab { Timeline, Heatmap, Books };

  // One row of the Books breakdown tab: a book that has accrued reading time and
  // its cross-device total (this device + last-synced other devices). Title is
  // read from the cache label sidecar filename — no book.bin parse.
  struct BookStatRow {
    std::string title;
    uint32_t allDevicesSeconds = 0;  // BookReadingStats::displayTotalSeconds()
    std::string dirName;             // cache dir under /.crosspoint (delete target)
  };

  // ~1.7 KB (embeds two ReadingTimeHistory: local + remote snapshot) —
  // heap-allocated, never a stack local or by-value member (see
  // GlobalReadingStats.h / ReadingTimeHistory.h).
  std::unique_ptr<GlobalReadingStats> stats;
  // Cross-device display history: local overlaid with the synced remote snapshot,
  // built once in onEnter() (the heatmap renders every frame — don't re-fold per
  // frame). Equals the local history when nothing has been synced.
  std::unique_ptr<ReadingTimeHistory> displayHist;

  // Per-book breakdown for the Books tab, sorted by descending time. Built once in
  // onEnter() (a directory scan + one stats.bin read per book — never per frame).
  std::vector<BookStatRow> bookRows;
  // Sum of every row's allDevicesSeconds. Shown as the pinned reconciliation
  // subtotal: it need not equal the headline total (that is an independent
  // monotonic counter), and the gap is the point of this screen.
  uint32_t booksSumSeconds = 0;
  int booksScrollOffset = 0;
  // Selected row on the Books tab (long-press Confirm deletes its whole cache dir).
  int booksSelectedIndex = 0;
  // Swallow the Confirm release after a long-press has fired so it doesn't re-trigger.
  bool booksLongPressFired = false;

  // On-screen tab order, shared by the Left/Right cycle and the touch hit test.
  static constexpr Tab TAB_ORDER[] = {Tab::Timeline, Tab::Heatmap, Tab::Books};
  static constexpr int TAB_COUNT = static_cast<int>(sizeof(TAB_ORDER) / sizeof(TAB_ORDER[0]));

  Tab selectedTab = Tab::Timeline;
  // Shared Timeline/Heatmap presentation (stacked Yearly/Monthly/Weekly + scroll).
  StatsTimelineView timeline;

  ButtonNavigator buttonNavigator;

  // Area below the tab bar shared by all tabs; single source of truth so loop()'s
  // scroll clamping and render()'s drawing always agree on available height.
  Rect contentRect() const;
  // Band the tab labels are drawn in. contentRect() starts below it, and the
  // touch hit test measures against it, so both follow the summary block.
  Rect tabBarRect() const;
  // Tab labels + which one is selected, shared by the draw and the hit test so
  // the touch bands cannot drift from the painted labels.
  std::vector<TabInfo> buildTabs() const;
  // Switch to the tab at `index` in the on-screen order, resetting the per-tab
  // cursors the same way the Left/Right cycle does.
  void selectTab(int index);
  // Tab taps, Books row taps/holds and Timeline cell taps. Returns true when the
  // pass is consumed. Touch boards only.
  bool handleTouch();

  // Scans /.crosspoint for book caches with reading time and fills bookRows +
  // booksSumSeconds. Device-only (uses Storage); no-op result on host/empty card.
  void buildBookBreakdown();
  void renderBooksTab(const Rect& content) const;
  int booksVisibleRows(const Rect& content) const;
  int booksMaxScroll(const Rect& content) const;
  // Keeps booksSelectedIndex within the scroll window after a selection move.
  void scrollSelectedBookIntoView(const Rect& content);
  // Confirms, then clears the selected book so it reads as never opened: its whole cache
  // dir (progress + sections + cover + stats), bookmarks, recent-books entry and the
  // resume pointer. The book file itself is kept. Drops the row locally on success — no
  // rescan.
  void promptDeleteBook();
  // Opens the selected row's book in the reader. The row only knows its path-hash cache
  // dir, so the book path is read from that dir's content_id.bin on demand (one SD read
  // per press — never per row at scan time). Toasts instead of navigating when the path
  // is unknown or the file is gone.
  void openSelectedBook();
};
