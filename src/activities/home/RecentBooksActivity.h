#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Set when a long-press has fired; input is swallowed until Confirm is released
  // again so the release doesn't also open the book.
  bool longPressFired = false;

  // Reorder gesture state: hold Left = move selected book up, hold Right = down.
  // reorderActive swallows input until the held button is released; reorderDirty
  // tracks whether any swap happened so we persist once when the gesture ends.
  bool reorderActive = false;
  bool reorderDirty = false;
  unsigned long lastReorderMs = 0;

  // Recent tab state
  std::vector<RecentBook> recentBooks;

  // Data loading
  void loadRecentBooks();

  // Move the selected entry up/down one slot (in-memory). Returns false if at the
  // boundary. Updates selectorIndex + repaints; persistence is deferred to release.
  bool moveSelectedUp();
  bool moveSelectedDown();

  // Show an OK/Cancel prompt to remove the given book from the Recent Books list.
  void promptRemoveBook(const std::string& path, const std::string& title);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
