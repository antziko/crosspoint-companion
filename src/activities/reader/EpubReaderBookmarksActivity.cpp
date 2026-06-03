#include "EpubReaderBookmarksActivity.h"

#include "../../BookmarkStore.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;

// Layout constants used in renderScreen
constexpr int LINE_HEIGHT = 60;
}  // namespace

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();

  bookmarks = BOOKMARKS.getBookmarks();
  LOG_DBG("EPB", "Loaded %d bookmarks", static_cast<int>(bookmarks.size()));

  requestUpdate();
}

void EpubReaderBookmarksActivity::onExit() { Activity::onExit(); }

int EpubReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;
}

int EpubReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) {
  const auto pageHeight = renderer.getScreenHeight();
  return pageHeight - getGutterBottom(renderer) - LINE_HEIGHT;
}

void EpubReaderBookmarksActivity::loop() {
  // Delete confirmation mode
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmingDelete == DELETE_MODE_DISPLAY) {
        confirmingDelete = DELETE_MODE_CONFIRM;
        requestUpdate();
        return;
      }
      BOOKMARKS.removeBookmarkAt(static_cast<size_t>(selectorIndex));
      bookmarks = BOOKMARKS.getBookmarks();

      if (selectorIndex >= static_cast<int>(bookmarks.size()) && selectorIndex > 0) {
        selectorIndex--;
      }

      requestUpdate();
      confirmingDelete = DELETE_MODE_OFF;
      return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      requestUpdate();
      confirmingDelete = DELETE_MODE_OFF;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (bookmarks.empty()) {
      return;
    }
    const struct Bookmark& bm = bookmarks.at(static_cast<size_t>(selectorIndex));
    setResult(BookmarkResult{bm.spineIndex, bm.progress, bm.paragraphIndex});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
    if (bookmarks.empty()) {
      return;
    }
    confirmingDelete = DELETE_MODE_DISPLAY;
    requestUpdate();
  }

  buttonNavigator.onNextRelease([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, bookmarks.size());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, bookmarks.size(),
                                                   GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, bookmarks.size(),
                                                       GUI.getListPageItems(getListHeight(renderer), true));
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = getGutterBottom(renderer);
  const int contentY = hintGutterHeight;
  const int listY = contentY + LINE_HEIGHT;
  const int listHeight = getListHeight(renderer);
  const int numBookmarks = static_cast<int>(bookmarks.size());

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const auto getBookmarkTitle = [this](int index) -> std::string {
    const struct Bookmark& bm = bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    return bm.snippet[0] != '\0' ? std::string(bm.snippet) : std::string(tr(STR_BOOKMARK_INSTRUCTIONS));
  };
  const auto getBookmarkSubtitle = [this](int index) -> std::string {
    const struct Bookmark& bm = bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    const char* chapter = bm.chapterTitle[0] != '\0' ? bm.chapterTitle : tr(STR_UNNAMED);
    const int pct = static_cast<int>(std::lround(bm.progress * 100.0f));
    char buf[96];
    if (bm.chapterPageCount > 0) {
      // Snapshot page position within the chapter (chapterCurrentPage is 0-based).
      snprintf(buf, sizeof(buf), "%d%% - %d/%d - %s", pct, bm.chapterCurrentPage + 1, bm.chapterPageCount, chapter);
    } else {
      snprintf(buf, sizeof(buf), "%d%% - %s", pct, chapter);
    }
    return std::string(buf);
  };
  const auto getBookmarkIcon = [this](int index) {
    const struct Bookmark& bm =
        bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    return bm.returnMark ? UIIcon::BookmarkReturn : UIIcon::Bookmark;
  };

  if (numBookmarks > 0) {
    if (confirmingDelete >= DELETE_MODE_DISPLAY) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_CONFIRM_DELETE_BOOKMARK));

      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getBookmarkTitle,
                   getBookmarkSubtitle, getBookmarkIcon);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                   getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon);

      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_CONFIRM_TO_DELETE));
    }
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                     tr(STR_BOOKMARK_INSTRUCTIONS));
  }

  const auto backLabel = confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel =
      bookmarks.size() > 0 ? (confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_DELETE) : tr(STR_OPEN)) : "";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
