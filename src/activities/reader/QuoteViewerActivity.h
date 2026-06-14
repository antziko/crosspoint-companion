#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../Activity.h"

// Full-screen reader for a saved quote ("highlight"). Launched from
// EpubReaderBookmarksActivity when Confirm is pressed on a quote row; point
// bookmarks still jump directly without opening the viewer.
//
// The full preview text is NOT held resident across the bookmark set: the viewer
// reads exactly one quote's text from the .qtext sidecar (BookmarkStore::readPreviewAt)
// at a time, and reads the bookmark metadata from the BOOKMARKS singleton by index —
// so it never copies the bookmarks vector.
//
// Navigation:
//   Up/Down    prev / next quote (wraps)
//   Left/Right prev / next screen-page of the current quote (for long previews)
//   Confirm    emit BookmarkResult{spine, progress} → caller jumps to the quote
//   Back       cancel (caller keeps its list cursor)
class QuoteViewerActivity final : public Activity {
 public:
  QuoteViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int initialBookmarkIndex);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Reload preview text + rewrap for the quote at quoteIndices_[currentPos_].
  void loadCurrent();

  // Absolute indices (into BOOKMARKS.getBookmarks()) of the quote rows, in list order.
  std::vector<size_t> quoteIndices_;
  int currentPos_ = 0;   // position within quoteIndices_
  int pageOffset_ = 0;   // first visible wrapped-line index

  std::string previewText_;              // current quote's full text (only one resident)
  std::vector<std::string> wrappedLines_;
  int lineHeight_ = 0;
  int linesPerPage_ = 0;
};
