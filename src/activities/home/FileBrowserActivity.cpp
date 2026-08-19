#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
// Default window size when the on-screen row count can't be derived yet. The real size
// comes from windowCapacity() (rows that fit one screen).
constexpr size_t DEFAULT_WINDOW = 12;
// Yield to the scheduler every N entries during a directory scan so a folder with tens of
// thousands of files cannot trip the task watchdog.
constexpr size_t SCAN_YIELD_EVERY = 64;

// Strict TOTAL order over entry names: natural order, with a raw byte-compare tiebreak so
// two distinct names are never "equal". Cursor paging relies on this — a non-strict order
// could skip or duplicate an entry at a page boundary.
bool entryNameLess(const std::string& a, const std::string& b) {
  if (FsHelpers::naturalFileLess(a, b)) return true;
  if (FsHelpers::naturalFileLess(b, a)) return false;
  return a < b;
}
}  // namespace

bool FileBrowserActivity::accepts(const char* name, bool isDir) const {
  if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
    return false;
  }
  if (isDir) return true;
  std::string_view fn{name};
  if (mode == Mode::PickFirmware) return FsHelpers::checkFileExtension(fn, ".bin");
  return FsHelpers::hasEpubExtension(fn) || FsHelpers::hasXtcExtension(fn) || FsHelpers::hasTxtExtension(fn) ||
         FsHelpers::hasMarkdownExtension(fn) || FsHelpers::hasBmpExtension(fn) || FsHelpers::hasPngExtension(fn);
}

// Rows that fit one screen. Must mirror render()'s content rect exactly so drawList's
// internal paging (rect.height / rowHeight) yields the same row count as the loaded window
// — otherwise the window holds rows that spill onto a drawList sub-page. The safe area is
// orientation-aware: in landscape the front button-hints take screen WIDTH (drawn on a
// physical side edge), not bottom height, so the list keeps full height there.
size_t FileBrowserActivity::windowCapacity() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int pathY = screen.height - metrics.verticalSpacing - pathLineHeight;
  const int contentHeight = (pathY - metrics.verticalSpacing) - contentTop;
  const int items = (contentHeight > 0) ? contentHeight / metrics.listRowHeight : 0;
  return (items > 0) ? static_cast<size_t>(items) : DEFAULT_WINDOW;
}

// Single directory scan: feed every matching entry to a bounded WindowSelector (RAM ≤ one
// window) and record the global min/max name so has-prev / has-next are known without a
// second scan. `files` ends up holding just the selected window, in sorted order.
void FileBrowserActivity::loadWindow(filewindow::WindowSelector::Mode mode_, std::string cursor) {
  files.clear();
  winFirst.clear();
  winLast.clear();
  hasPrev = false;
  hasNext = false;
  totalMatches = 0;
  totalFiles = 0;
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return;
  }

  filewindow::WindowSelector sel(mode_, cursor, windowCapacity(), &entryNameLess);
  std::string globalMin, globalMax;
  bool haveBounds = false;
  size_t lessThanCursor = 0;       // matches sorting strictly before `cursor` (for the global rank)
  size_t filesLessThanCursor = 0;  // non-dir matches strictly before `cursor` (for the files-only number)

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) return;
  root.rewindDirectory();
  size_t scanned = 0;
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const bool isDir = file.isDirectory();
    if (accepts(fileNameBuffer.get(), isDir)) {
      filewindow::Entry e;
      e.name = isDir ? (std::string(fileNameBuffer.get()) + "/") : std::string(fileNameBuffer.get());
      e.size = isDir ? 0u : static_cast<uint32_t>(file.size());
      e.isDir = isDir;
      ++totalMatches;
      if (!isDir) ++totalFiles;
      if (!cursor.empty() && entryNameLess(e.name, cursor)) {
        ++lessThanCursor;
        if (!isDir) ++filesLessThanCursor;
      }
      if (!haveBounds) {
        globalMin = globalMax = e.name;
        haveBounds = true;
      } else {
        if (entryNameLess(e.name, globalMin)) globalMin = e.name;
        if (entryNameLess(globalMax, e.name)) globalMax = e.name;
      }
      sel.consider(e);
    }
    if ((++scanned % SCAN_YIELD_EVERY) == 0) vTaskDelay(1);
  }
  root.close();

  const auto& win = sel.window();
  files.reserve(win.size());
  for (const auto& e : win) files.push_back({e.name, e.size});
  if (files.empty()) {
    windowStartRank = 0;
    windowStartFileRank = 0;
    return;
  }

  // Files-only count within the loaded window (dirs carry a trailing '/').
  size_t filesInWindow = 0;
  for (const auto& f : files) {
    if (f.name.empty() || f.name.back() != '/') ++filesInWindow;
  }
  // The cursor entry (a real prior window edge for After mode) is a file iff its name
  // has no trailing '/'. After mode counts it as before the new window's first row.
  const bool cursorIsFile = !cursor.empty() && cursor.back() != '/';

  winFirst = files.front().name;
  winLast = files.back().name;
  // A page exists in a direction iff the window edge isn't the global edge.
  hasPrev = entryNameLess(globalMin, winFirst);
  hasNext = entryNameLess(winLast, globalMax);

  // 0-based global rank of the window's first row, from the single scan above. `cursor`
  // for After/Before is a real matching entry (a prior window edge), so it is counted.
  switch (mode_) {
    case filewindow::WindowSelector::Mode::First:
      windowStartRank = 0;
      windowStartFileRank = 0;
      break;
    case filewindow::WindowSelector::Mode::Last:
      windowStartRank = totalMatches - files.size();
      windowStartFileRank = totalFiles - filesInWindow;
      break;
    case filewindow::WindowSelector::Mode::AtOrAfter:
      windowStartRank = lessThanCursor;
      windowStartFileRank = filesLessThanCursor;
      break;
    case filewindow::WindowSelector::Mode::After:
      windowStartRank = lessThanCursor + 1;  // skip the cursor entry itself
      windowStartFileRank = filesLessThanCursor + (cursorIsFile ? 1 : 0);
      break;
    case filewindow::WindowSelector::Mode::Before:
      windowStartRank = (lessThanCursor >= files.size()) ? lessThanCursor - files.size() : 0;
      windowStartFileRank = (filesLessThanCursor >= filesInWindow) ? filesLessThanCursor - filesInWindow : 0;
      break;
  }

  // One SD pass for every CJK filename now in the window; the repaints that follow (cursor
  // steps, tap flashes) then hit the resident tables instead of re-reading per string. Getter
  // form: no concatenated copy, whose bare-new growth is what abort()s under heap pressure.
  // Rows draw in UI_10_FONT_ID (BaseTheme::drawList); the path band below them draws in
  // SMALL_FONT_ID and is warmed separately -- a different font id means a different arena, so
  // folding it into the row batch would not have covered it.
  renderer.prewarmFallbackText(
      UI_10_FONT_ID,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<FileEntry>*>(ctx))[i].name.c_str();
      },
      &files, static_cast<uint32_t>(files.size()));
  renderer.prewarmFallbackText(SMALL_FONT_ID, basepath.c_str());
}

void FileBrowserActivity::loadFirstWindow() {
  loadWindow(filewindow::WindowSelector::Mode::First, "");
  selectorIndex = 0;
}

void FileBrowserActivity::loadLastWindow() {
  loadWindow(filewindow::WindowSelector::Mode::Last, "");
  selectorIndex = files.empty() ? 0 : files.size() - 1;
}

void FileBrowserActivity::loadWindowContaining(const std::string& name) {
  loadWindow(filewindow::WindowSelector::Mode::AtOrAfter, name);
  // AtOrAfter anchors the window to START at `name`, dropping every sibling that sorts
  // before it. When `name` actually falls on the first page (rank < one window), that
  // hides earlier siblings the user expects to still see (e.g. returning from a folder
  // into a parent whose folders all fit one screen). In that case reload from the top so
  // the natural first page is shown, then place the cursor on the target.
  if (windowStartRank < windowCapacity()) {
    loadWindow(filewindow::WindowSelector::Mode::First, "");
  }
  // Land the cursor on the requested entry when present; otherwise default to the top.
  selectorIndex = 0;
  for (size_t i = 0; i < files.size(); i++) {
    if (files[i].name == name) {
      selectorIndex = i;
      break;
    }
  }
}

void FileBrowserActivity::reloadCurrentWindow() {
  // Re-pull the window that starts at the current top (used after a delete / hidden toggle).
  const std::string anchor = winFirst;
  if (anchor.empty()) {
    loadFirstWindow();
    return;
  }
  loadWindow(filewindow::WindowSelector::Mode::AtOrAfter, anchor);
  if (files.empty()) {
    loadLastWindow();  // the whole tail was deleted — fall back to the new last page
    return;
  }
  if (selectorIndex >= files.size()) selectorIndex = files.size() - 1;
}

void FileBrowserActivity::pageDown() {
  if (hasNext) {
    loadWindow(filewindow::WindowSelector::Mode::After, winLast);
    selectorIndex = 0;
  } else {
    loadFirstWindow();  // wrap to the top
  }
}

void FileBrowserActivity::pageUp() {
  if (hasPrev) {
    loadWindow(filewindow::WindowSelector::Mode::Before, winFirst);
    selectorIndex = files.empty() ? 0 : files.size() - 1;
  } else {
    loadLastWindow();  // wrap to the bottom
  }
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  // One of the few non-reader screens that follows SETTINGS.displayOrientation
  // (the hold-to-rotate gesture is handled in loop(), see resolveSideNavAction).
  ReaderUtils::applyOrientation(renderer, SETTINGS.displayOrientation);

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  selectorIndex = 0;

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFirstWindow();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    loadWindowContaining(fileName);  // open the window holding the previously-selected file
  } else {
    loadFirstWindow();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  files.clear();
  fileNameBuffer.reset();
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

// Every mutation of the list state below runs under a RenderLock.
//
// render() executes on the ActivityManager render task and reads `basepath`,
// `files[i].name`/`.size`, `selectorIndex`, `totalFiles` and
// `windowStartFileRank`; loop() runs on the main task and rebuilds all of them
// through loadWindow(), which clears and refills `files`. ActivityManager
// deliberately leaves the locking to the activity, and this screen never took
// it. Upstream decoded the resulting crash on hardware: the render task's row
// lambda called getFileExtension() on a freed std::string, rfind('.') over
// garbage returned npos, and substr(npos) aborted (#3034). The window scales
// with render time, so the CJK SD-fallback path widens it considerably.
//
// The lock is always released before requestUpdate(), finish(), onSelectBook(),
// onGoHome() and startActivityForResult*(): RenderLock wraps a plain
// (non-recursive) mutex taken with portMAX_DELAY, so holding it across an
// activity transition deadlocks rather than fails.
void FileBrowserActivity::loop() {
  // Hold Back at root (button reads "Home") toggles show-hidden-files and reloads the list.
  if (mode == Mode::Books && basepath == "/" && !lockLongPressBack && !hiddenToggleFired &&
      mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    hiddenToggleFired = true;
    SETTINGS.showHiddenFiles = !SETTINGS.showHiddenFiles;
    SETTINGS.saveToFile();
    {
      RenderLock lock(*this);
      loadFirstWindow();  // the visible set changed; restart from the top
    }
    requestUpdate(true);
    return;
  }
  // Swallow the Back release that ends the hold so the short-press "go home" does not also fire.
  if (hiddenToggleFired) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) hiddenToggleFired = false;
    return;
  }

  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/" && !lockLongPressBack) {
    {
      RenderLock lock(*this);
      basepath = "/";
      loadFirstWindow();
    }
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (files.empty()) return;
    // An activation can carry a row index captured before a delete or a reload
    // shrank the list; the next render re-registers the rows.
    if (selectorIndex >= files.size()) return;

    const std::string& entry = files[selectorIndex].name;
    bool isDirectory = (entry.back() == '/');

    // Firmware picker: select file -> return path; navigate into directories normally.
    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE OR DIRECTORY ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + entry;

      auto handler = [this, fullPath](const ActivityResult& res) {
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (removeDirFile(fullPath)) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            // Drop any recent-books entry whose backing file is now gone -- the
            // deleted book, or every book under a deleted folder. Same prune the
            // Recent Books screen uses (RecentBooksActivity).
            if (RECENT_BOOKS.pruneMissing()) {
              RECENT_BOOKS.saveToFile();
            }
            {
              RenderLock lock(*this);
              reloadCurrentWindow();  // re-pull the window around the current position
            }
            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");

      startActivityForResultNoThrow<ConfirmationActivity>(handler, renderer, mappedInput, heading, entry);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (isDirectory) {
        {
          RenderLock lock(*this);
          // `entry` is a reference INTO files[], which loadFirstWindow() clears
          // — read it into basepath before the load, never after.
          if (basepath.back() != '/') basepath += "/";
          basepath += entry.substr(0, entry.length() - 1);
          loadFirstWindow();
        }
        requestUpdate();
      } else {
        std::string fullPath;
        {
          RenderLock lock(*this);
          if (basepath.back() != '/') basepath += "/";
          fullPath = basepath + entry;  // own the string: `entry` points into files[]
        }
        onSelectBook(fullPath);  // launches an activity: never under the lock
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;
        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        {
          RenderLock lock(*this);
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadWindowContaining(dirName);  // restore selection onto the folder we came out of
        }
        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  // Single-step within the window; crossing an edge loads the adjacent window (which wraps
  // around at the very ends). `files` is one screen, so a step past the edge is a page-turn.
  const auto navigateNext = [this] {
    {
      RenderLock lock(*this);
      if (files.empty()) return;
      if (selectorIndex + 1 < files.size()) {
        selectorIndex++;
      } else {
        pageDown();  // rebuilds files[]
      }
    }
    requestUpdate();
  };
  const auto navigatePrevious = [this] {
    {
      RenderLock lock(*this);
      if (files.empty()) return;
      if (selectorIndex > 0) {
        selectorIndex--;
      } else {
        pageUp();  // rebuilds files[]
      }
    }
    requestUpdate();
  };

  // Front Left/Right: single-step on release + continuous page-jump (whole window) while held.
  buttonNavigator.onRelease({MappedInputManager::Button::Right}, navigateNext);
  buttonNavigator.onRelease({MappedInputManager::Button::Left}, navigatePrevious);
  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this] {
    {
      RenderLock lock(*this);
      pageDown();
    }
    requestUpdate();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this] {
    {
      RenderLock lock(*this);
      pageUp();
    }
    requestUpdate();
  });

  // Physical side Up/Down: single-step only (no continuous page-jump) -- holding
  // them is reserved for the display-orientation-cycle gesture.
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Down)) {
    case ReaderUtils::SideNavAction::STEP:
      navigateNext();
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, -1);
      {
        RenderLock lock(*this);
        reloadCurrentWindow();  // window capacity changed with orientation; re-pull from current top
      }
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Up)) {
    case ReaderUtils::SideNavAction::STEP:
      navigatePrevious();
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, 1);
      {
        RenderLock lock(*this);
        reloadCurrentWindow();  // window capacity changed with orientation; re-pull from current top
      }
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
}

std::string getFileName(std::string filename) {
  if (filename.empty()) return filename;
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);  // pos==npos (no dot) -> whole name, which is correct
}

std::string getFileExtension(const std::string& filename) {
  if (filename.empty() || filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  // No dot: substr(npos) would throw out_of_range -> abort under -fno-exceptions. Return no ext.
  if (pos == std::string::npos) return "";
  return filename.substr(pos);
}

// Human-readable size in MB (1 decimal); switches to GB past 1 GB. Sub-MB files
// still read as "0.x MB" — fine for ebooks, avoids a noisy KB/B unit.
std::string formatFileSize(uint32_t bytes) {
  char buf[16];
  if (bytes < 1024u * 1024u * 1024u) {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
  } else {
    snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
  }
  return std::string(buf);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  // Orientation-aware: in landscape the front button-hints occupy a physical side edge
  // (drawButtonHints forces portrait internally), so the safe area reserves WIDTH there and
  // keeps full height — no wasted bottom padding, and the row count matches windowCapacity().
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  // Append the folder's total file count (whole folder, not just the loaded window).
  char countBuf[24];
  snprintf(countBuf, sizeof(countBuf), " (%u)", static_cast<unsigned>(totalFiles));
  folderName += countBuf;
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int pathY = screen.y + screen.height - metrics.verticalSpacing - pathLineHeight;
  const int contentHeight = (pathY - metrics.verticalSpacing) - contentTop;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, screen.x + metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, files.size(), selectorIndex,
        [this](int index) {
          // Number files only (folders are unnumbered), continuous across pages: 1., 2., …
          // over the whole folder. The files-only rank skips interspersed directories.
          const bool isDir = !files[index].name.empty() && files[index].name.back() == '/';
          if (isDir) return getFileName(files[index].name);
          size_t filesBefore = 0;
          for (int i = 0; i < index; i++) {
            if (files[i].name.empty() || files[i].name.back() != '/') ++filesBefore;
          }
          char prefix[16];
          snprintf(prefix, sizeof(prefix), "%u. ", static_cast<unsigned>(windowStartFileRank + filesBefore + 1));
          return prefix + getFileName(files[index].name);
        },
        nullptr, [this](int index) { return UITheme::getFileIcon(files[index].name); },
        // Trailing value: "<ext>  <size>" on one line (directories show nothing).
        [this](int index) {
          const std::string ext = getFileExtension(files[index].name);
          if (ext.empty()) return std::string("");
          return ext + "  " + formatFileSize(files[index].size);
        },
        false, nullptr, /*valueSmallFont=*/true);
  }

  // Full path display
  {
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(screen.x, separatorY, screen.x + screen.width - 1, separatorY, 3, true);
    const int pathMaxWidth = screen.width - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, screen.x + metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && selectorIndex < files.size() &&
                                     files[selectorIndex].name.back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // FAST_REFRESH is a differential waveform that resolves cleanly only in the
  // panel's native portrait scan direction; in landscape repeated up/down
  // accumulates DC bias into progressive whitening. Keep FAST (snappy) and scrub
  // with one HALF every N moves (N = the user's Refresh Frequency) instead of
  // paying HALF on every move. Portrait stays pure FAST (no washout there).
  const auto orient = renderer.getOrientation();
  const bool landscape = orient == GfxRenderer::Orientation::LandscapeClockwise ||
                         orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Entry render scrubs (counter starts at 0) for a clean baseline, then FAST until
  // the next periodic HALF (insurance for the X3 turbo path; X4 FAST self-resyncs).
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  if (landscape && --pagesUntilFullRefresh <= 0) {
    mode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = std::max(1, SETTINGS.getRefreshFrequency());
  }
  renderer.displayBuffer(mode);
}
