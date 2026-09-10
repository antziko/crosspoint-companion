#include "TxtReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;

constexpr int LINE_HEIGHT = 60;
}  // namespace

void TxtReaderBookmarksActivity::onEnter() {
  Activity::onEnter();
  bookmarks = TxtBookmarkStore::load(cachePath);
  LOG_DBG("TBV", "Loaded %d txt bookmarks", static_cast<int>(bookmarks.size()));
  requestUpdate();
}

void TxtReaderBookmarksActivity::onExit() { Activity::onExit(); }

int TxtReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) {
  const bool isPortrait = renderer.getOrientation() == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;
}

int TxtReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) const {
  return renderer.getScreenHeight() - getGutterBottom(renderer) - LINE_HEIGHT;
}

void TxtReaderBookmarksActivity::loop() {
  // Delete confirmation mode
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmingDelete == DELETE_MODE_DISPLAY) {
        confirmingDelete = DELETE_MODE_CONFIRM;
        requestUpdate();
        return;
      }
      {
        // render() indexes bookmarks with .at(), which aborts under
        // -fno-exceptions when the index is stale -- so erasing here while the
        // render task is mid-row is a reboot, not a glitch. Same race as
        // FileBrowserActivity (#3034); released before requestUpdate().
        RenderLock lock(*this);
        if (selectorIndex >= 0 && selectorIndex < static_cast<int>(bookmarks.size())) {
          bookmarks.erase(bookmarks.begin() + selectorIndex);
          TxtBookmarkStore::save(cachePath, bookmarks);
        }
        if (selectorIndex >= static_cast<int>(bookmarks.size()) && selectorIndex > 0) {
          selectorIndex--;
        }
        confirmingDelete = DELETE_MODE_OFF;
      }
      requestUpdate();
      return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      confirmingDelete = DELETE_MODE_OFF;
      requestUpdate();
      return;
    }
  }

  // A tap on a row opens that bookmark. A tap is never a hold, so it cannot reach
  // the hold-to-delete branch above.
  int tapX = 0;
  int tapY = 0;
  const int tappedRow = mappedInput.wasScreenTapped(tapX, tapY) ? listTouch_.indexAt(renderer, tapX, tapY) : -1;
  if (tappedRow >= 0 && tappedRow < static_cast<int>(bookmarks.size())) selectorIndex = tappedRow;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || tappedRow >= 0) {
    if (bookmarks.empty()) {
      return;
    }
    setResult(PageResult{bookmarks.at(static_cast<size_t>(selectorIndex)).page});
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

void TxtReaderBookmarksActivity::render(RenderLock&&) {
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
    const TxtBookmark& bm =
        bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    return bm.snippet[0] != '\0' ? std::string(bm.snippet) : std::string(tr(STR_BOOKMARK_INSTRUCTIONS));
  };
  const auto getBookmarkSubtitle = [this](int index) -> std::string {
    const TxtBookmark& bm =
        bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    const int pct = totalPages > 0 ? static_cast<int>((bm.page + 1) * 100.0f / totalPages + 0.5f) : 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "%d%% - %u/%d", pct, bm.page + 1, totalPages);
    return std::string(buf);
  };
  const auto getBookmarkIcon = [](int) { return UIIcon::BookmarkRibbon; };

  if (numBookmarks > 0) {
    if (confirmingDelete >= DELETE_MODE_DISPLAY) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_CONFIRM_DELETE_BOOKMARK));
      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getBookmarkTitle,
                   getBookmarkSubtitle, getBookmarkIcon);
      // The confirm view's single row is a preview of what is about to be deleted,
      // not something to pick.
      listTouch_.clear();
    } else {
      listTouch_.record(Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                        /*hasSubtitle=*/true);
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                   getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon);
      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_CONFIRM_TO_DELETE));
    }
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                     tr(STR_BOOKMARK_INSTRUCTIONS));
    listTouch_.clear();
  }

  const auto backLabel = confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel =
      numBookmarks > 0 ? (confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_DELETE) : tr(STR_OPEN)) : "";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
