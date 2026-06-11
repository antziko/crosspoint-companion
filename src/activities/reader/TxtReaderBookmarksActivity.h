#pragma once

#include <string>
#include <vector>

#include "../../TxtBookmarkStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Bookmark list for the plain-text (.txt) reader. Mirrors the EPUB bookmark
// viewer UX (scroll, open, hold-Confirm to delete) but over TxtBookmark entries
// (page index + snippet) instead of EPUB spine/chapter bookmarks.
//
// Returns PageResult{page} on open, or a cancelled result on Back.
class TxtReaderBookmarksActivity final : public Activity {
 public:
  explicit TxtReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string cachePath,
                                      int totalPages)
      : Activity("TxtReaderBookmarks", renderer, mappedInput),
        cachePath(std::move(cachePath)),
        totalPages(totalPages) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string cachePath;
  int totalPages = 0;

  std::vector<TxtBookmark> bookmarks;
  int selectorIndex = 0;
  int confirmingDelete = 0;  // 0 = off, 1 = display prompt, 2 = confirm
  ButtonNavigator buttonNavigator;

  static int getGutterBottom(const GfxRenderer& renderer);
  int getListHeight(const GfxRenderer& renderer) const;
};
