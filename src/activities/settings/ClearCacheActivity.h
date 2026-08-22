#pragma once

#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/BookCacheUtils.h"

class ClearCacheActivity final : public Activity {
 public:
  // ClearAll wipes every book cache dir (all progress lost). PruneOrphans removes only
  // caches whose book is no longer on the SD card, keeping live books' progress/stats.
  // RepaginateAll deletes just each book's sections/ directory -- the cached page layout --
  // so every book re-flows on next open while progress, metadata and covers survive. That is
  // the recovery path when a chapter was laid out wrongly (e.g. a mid-build heap dip left it
  // unstyled) and the layout, not the book, is what needs discarding.
  enum class Mode { ClearAll, PruneOrphans, RepaginateAll };

  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::ClearAll)
      : Activity("ClearCache", renderer, mappedInput), mode_(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, SCANNING, PREVIEW, CLEARING, SUCCESS, FAILED };

  State state = WARNING;
  Mode mode_ = Mode::ClearAll;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  void clearCache();

  // PruneOrphans flow: scan (preview) -> confirm -> remove.
  CachePruneResult scanResult_;
  std::vector<std::string> orphanDirs_;
  void scanOrphans();
  void removeScannedOrphans();
};
