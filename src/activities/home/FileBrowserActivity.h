#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

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
  // during loadFiles() — no extra SD I/O — and kept beside the name so sorting
  // never desyncs the two. +4 bytes/entry over a bare name.
  struct FileEntry {
    std::string name;
    uint32_t size = 0;
  };

  // Files state
  std::string basepath = "/";
  std::vector<FileEntry> files;
  std::unique_ptr<char[]> fileNameBuffer;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

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
