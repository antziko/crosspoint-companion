#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/FileWindowSelect.h"

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Deletion
  bool removeDirFile(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  bool hiddenToggleFired = false;  // swallow Back release after show-hidden-files toggle fires
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  Mode mode = Mode::Books;

  // One listed entry: display name (directories keep a trailing "/") plus the
  // file size in bytes (0 for directories). Size is read from the directory entry
  // during the window scan — no extra SD I/O — and kept beside the name so sorting
  // never desyncs the two. +4 bytes/entry over a bare name.
  struct FileEntry {
    std::string name;
    uint32_t size = 0;
  };

  // Files state. `files` holds only ONE on-screen window of the folder, not the whole
  // listing, so RAM is bounded regardless of how many files the folder has (the fix for
  // OOM-aborts on huge folders). The directory is re-scanned once per page-turn and a
  // bounded WindowSelector keeps just the visible window in sorted order.
  std::string basepath = "/";
  std::vector<FileEntry> files;  // current window, ascending, size <= windowCapacity()
  std::unique_ptr<char[]> fileNameBuffer;

  // Window cursors + paging state (derived each load).
  std::string winFirst;        // first name in the current window
  std::string winLast;         // last name in the current window
  bool hasPrev = false;        // a matching entry exists before winFirst
  bool hasNext = false;        // a matching entry exists after winLast
  size_t totalMatches = 0;     // total matching entries in the folder (dirs + files)
  size_t totalFiles = 0;       // total matching non-directory entries (for the title count)
  size_t windowStartRank = 0;  // 0-based global rank of the window's first row (for the "#N." prefix)

  // Window loading. loadWindow() does the single directory scan; the wrappers pick the mode.
  void loadWindow(filewindow::WindowSelector::Mode mode, const std::string& cursor);
  void loadFirstWindow();                               // top of the folder
  void loadLastWindow();                                // bottom of the folder
  void loadWindowContaining(const std::string& name);   // window starting at `name` (position restore)
  void reloadCurrentWindow();                           // re-load around the current top (after delete/toggle)
  void pageDown();                                      // next window (wraps to first)
  void pageUp();                                        // previous window (wraps to last)
  size_t windowCapacity() const;                        // on-screen rows = window size
  bool accepts(const char* name, bool isDir) const;     // shared list filter

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
