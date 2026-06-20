#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <variant>

#include "../../BookmarkStore.h"
#include "MappedInputManager.h"
#include "QuoteViewerActivity.h"
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
    // Quote rows open the full-text viewer (which forwards a jump on its own Confirm);
    // point bookmarks jump straight to their page.
    if (bm.isQuote()) {
      startActivityForResult(std::make_unique<QuoteViewerActivity>(renderer, mappedInput, selectorIndex),
                             [this](const ActivityResult& r) {
                               if (!r.isCancelled) {
                                 if (const auto* br = std::get_if<BookmarkResult>(&r.data)) {
                                   setResult(ActivityResult{*br});
                                   finish();
                                   return;
                                 }
                               }
                               requestUpdate();
                             });
      return;
    }
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

  // Quote rows show 2 lines of the highlight teaser (title = line 1, subtitle = line 2)
  // by wrapping the resident 64-char snippet — no extra RAM, no .qtext read. The chapter/%
  // subtitle is kept only when the snippet fits one line (and for point bookmarks).
  //
  // The wrap MUST use UI_10_FONT_ID — the font drawList draws the title (line 1) in — or a
  // narrower-font break overflows when re-rendered larger and gets ellipsized mid-first-line.
  // Width must stay under the row's text column: contentWidth minus the side padding (20×2),
  // in-selection padding (8×2), the icon (32) + its gap (8), and the scroll-bar gutter (~9).
  // Budget ~110 conservatively so a full first line never spills into an ellipsis.
  const int snippetWrapW = std::max(80, contentWidth - 110);
  const auto getBookmarkTitle = [this, snippetWrapW](int index) -> std::string {
    const struct Bookmark& bm =
        bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    if (bm.isQuote() && bm.snippet[0] != '\0') {
      auto lines = renderer.wrappedText(UI_10_FONT_ID, bm.snippet, snippetWrapW, 2);
      return lines.empty() ? std::string(bm.snippet) : lines[0];
    }
    return bm.snippet[0] != '\0' ? std::string(bm.snippet) : std::string(tr(STR_BOOKMARK_INSTRUCTIONS));
  };
  const auto getBookmarkSubtitle = [this, snippetWrapW](int index) -> std::string {
    const struct Bookmark& bm =
        bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    if (bm.isQuote() && bm.snippet[0] != '\0') {
      auto lines = renderer.wrappedText(UI_10_FONT_ID, bm.snippet, snippetWrapW, 2);
      if (lines.size() > 1) return lines[1];  // second line of the highlight text
      // snippet fit one line — fall through to show chapter/% instead
    }
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
    if (bm.isQuote()) return UIIcon::Highlight;
    return bm.returnMark ? UIIcon::BookmarkReturn : UIIcon::Bookmark;
  };
  const auto getBookmarkSubtitleLarge = [this, snippetWrapW](int index) -> bool {
    const struct Bookmark& bm =
        bookmarks.at(static_cast<size_t>(confirmingDelete >= DELETE_MODE_DISPLAY ? selectorIndex : index));
    if (!bm.isQuote() || bm.snippet[0] == '\0') return false;
    return renderer.wrappedText(UI_10_FONT_ID, bm.snippet, snippetWrapW, 2).size() > 1;
  };

  if (numBookmarks > 0) {
    if (confirmingDelete >= DELETE_MODE_DISPLAY) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_CONFIRM_DELETE_BOOKMARK));

      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getBookmarkTitle,
                   getBookmarkSubtitle, getBookmarkIcon, nullptr, false, nullptr, false, getBookmarkSubtitleLarge);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                   getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon, nullptr, false, nullptr, false,
                   getBookmarkSubtitleLarge);

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
