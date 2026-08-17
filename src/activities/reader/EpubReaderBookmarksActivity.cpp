#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <variant>

#include "../../BookmarkStore.h"
#include "MappedInputManager.h"
#include "QuoteViewerActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long ENTER_DELETE_MODE_MS = 700;

// Width budget for the two-line quote wrap: the row's text column is the
// content band minus side padding, the 32px icon and its gap, and the scroll
// gutter. Kept as one conservative constant (rather than derived from the
// theme) because the wrap runs in rebuildRowItems(), outside buildScreen()
// where the screen's measured bands exist. Under-estimating is safe — the row
// just breaks a word earlier; over-estimating ellipsizes mid-line.
constexpr int ROW_TEXT_INSET = 110;
constexpr int MIN_WRAP_WIDTH = 80;
}  // namespace

EpubReaderBookmarksActivity::EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::string& epubPath)
    : UiListActivity("EpubReaderBookmarks", renderer, mappedInput, /*wantsTouchLongPress=*/true), epubPath(epubPath) {}

void EpubReaderBookmarksActivity::onEnter() {
  UiListActivity::onEnter();

  bookmarks = BOOKMARKS.getBookmarks();
  LOG_DBG("EPB", "Loaded %d bookmarks", static_cast<int>(bookmarks.size()));
  rebuildRowItems();
}

// Derives rowLabels/rowSubtitles/rowItems from `bookmarks`. Called whenever
// `bookmarks` changes (onEnter() load, post-delete) so buildScreen() reuses the
// cached rows on every repaint instead of re-wrapping every quote snippet and
// re-composing every subtitle per render.
void EpubReaderBookmarksActivity::rebuildRowItems() {
  rowLabels.clear();
  rowSubtitles.clear();
  rowItems.clear();
  rowLabels.reserve(bookmarks.size());
  rowSubtitles.reserve(bookmarks.size());
  rowItems.reserve(bookmarks.size());

  // Wrap in the font and weight the label actually draws in (buildScreen sets
  // labelText to a bold smallText), so a full first line can never come out
  // wider than the column and get ellipsized.
  const int smallFontId = uiScaleSpec().smallFontId;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int wrapWidth = std::max(MIN_WRAP_WIDTH, safe.width - ROW_TEXT_INSET);

  for (const auto& bm : bookmarks) {
    // Quote rows spend both text lines on the highlight teaser (label = line 1,
    // subtitle = line 2) by wrapping the resident 64-char snippet: no extra RAM
    // and no .qtext read. The chapter/% subtitle is kept only when the snippet
    // fits a single line — and always for point bookmarks.
    bool snippetUsesSubtitle = false;
    if (bm.isQuote() && bm.snippet[0] != '\0') {
      auto lines = renderer.wrappedText(smallFontId, bm.snippet, wrapWidth, 2, EpdFontFamily::BOLD);
      if (lines.empty()) {
        rowLabels.emplace_back(bm.snippet);
      } else {
        rowLabels.push_back(std::move(lines[0]));
        if (lines.size() > 1) {
          rowSubtitles.push_back(std::move(lines[1]));
          snippetUsesSubtitle = true;
        }
      }
    } else {
      rowLabels.emplace_back(bm.snippet[0] != '\0' ? bm.snippet : tr(STR_BOOKMARK_INSTRUCTIONS));
    }

    if (!snippetUsesSubtitle) {
      const char* chapter = bm.chapterTitle[0] != '\0' ? bm.chapterTitle : tr(STR_UNNAMED);
      const int pct = static_cast<int>(std::lround(bm.progress * 100.0f));
      char buf[96];
      if (bm.chapterPageCount > 0) {
        // Snapshot page position within the chapter (chapterCurrentPage is 0-based).
        snprintf(buf, sizeof(buf), "%d%% - %d/%d - %s", pct, bm.chapterCurrentPage + 1, bm.chapterPageCount, chapter);
      } else {
        snprintf(buf, sizeof(buf), "%d%% - %s", pct, chapter);
      }
      rowSubtitles.emplace_back(buf);
    }

    const UIIcon icon =
        bm.isQuote() ? UIIcon::Highlight : (bm.returnMark ? UIIcon::BookmarkReturn : UIIcon::BookmarkRibbon);

    fui::ListItem item;
    item.label = rowLabels.back().c_str();
    item.subtitle = rowSubtitles.back().c_str();
    item.icon = listIconFor(icon, 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void EpubReaderBookmarksActivity::openSelectedBookmark() {
  if (bookmarks.empty() || nav.selected < 0 || nav.selected >= listCount()) {
    return;
  }
  const Bookmark& bm = bookmarks[static_cast<size_t>(nav.selected)];
  // Quote rows open the full-text viewer (which forwards a jump on its own
  // Confirm); point bookmarks jump straight to their page.
  if (bm.isQuote()) {
    const int quoteIndex = nav.selected;
    startActivityForResultNoThrow<QuoteViewerActivity>(
        [this](const ActivityResult& r) {
          if (!r.isCancelled) {
            if (const auto* br = std::get_if<BookmarkResult>(&r.data)) {
              setResult(ActivityResult{*br});
              finish();
              return;
            }
          }
          requestUpdate();
        },
        renderer, mappedInput, quoteIndex);
    return;
  }
  setResult(BookmarkResult{bm.spineIndex, bm.progress, bm.paragraphIndex});
  finish();
}

void EpubReaderBookmarksActivity::activateIndex(const int index) {
  if (confirmPopup.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  // Activation opens the viewer or leaves this screen; a lingering flash would
  // gray an unrelated row on the next render.
  app.clearTapFlash();
  nav.selected = index;
  openSelectedBookmark();
}

void EpubReaderBookmarksActivity::onRowLongPress(const int index) {
  if (confirmPopup.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  // Touch long-press asks the same Cancel/Delete confirmation the physical
  // Confirm hold does, and does not open the bookmark.
  showDeleteConfirmation();
}

bool EpubReaderBookmarksActivity::handleCustomInput() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    // The popup acts on button press; if that input closed it, the trailing
    // release must be swallowed below (Back would leave the activity, Confirm
    // would open the selected bookmark).
    popupClosing = !confirmPopup.isActive();
    return true;
  }
  if (popupClosing) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      return true;  // closing press still held
    }
    popupClosing = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return true;  // swallow the release that closed the popup
    }
  }
  return false;
}

bool EpubReaderBookmarksActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedBookmark();
    return true;
  }

  // Hold Confirm on the selection: prompt to delete. No pass-consuming return —
  // the legacy loop also fell through to navigation after arming the prompt.
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
    showDeleteConfirmation();
  }

  return false;
}

void EpubReaderBookmarksActivity::showDeleteConfirmation() {
  if (bookmarks.empty() || confirmPopup.isActive()) {
    return;
  }
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_CONFIRM_DELETE_BOOKMARK), options, 2, 0, [this](const int idx) {
    if (idx == 1) {
      deleteSelectedBookmark();
    }
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderBookmarksActivity::deleteSelectedBookmark() {
  if (bookmarks.empty() || nav.selected < 0 || nav.selected >= listCount()) {
    return;
  }
  {
    // rowItems holds bare const char* into rowLabels/rowSubtitles, which
    // rebuildRowItems() clears and refills. The render task dereferences those
    // pointers, so the whole swap must be atomic against it (same race as
    // FileBrowserActivity, #3034). Released before requestUpdate().
    RenderLock lock(*this);
    BOOKMARKS.removeBookmarkAt(static_cast<size_t>(nav.selected));
    bookmarks = BOOKMARKS.getBookmarks();
    // Deleting shifts every later bookmark's index, so the cached labels,
    // subtitles and actionValues must be re-derived, not just trimmed.
    rebuildRowItems();

    if (nav.selected >= listCount() && nav.selected > 0) {
      nav.selected--;
    }
    nav.follow(listCount());
  }
  requestUpdate(true);
}

void EpubReaderBookmarksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band drawChrome() paints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (bookmarks.empty()) {
    screen.centeredText(tr(STR_BOOKMARK_INSTRUCTIONS), screen.theme().bodyText);
    return;
  }

  // "Hold Confirm to delete" names a physical button; on touch boards the row
  // long-press covers deletion, so the hint would be wrong there.
  if (!mappedInput.hasTouch()) {
    const int helpLineHeight = renderer.getLineHeight(uiScaleSpec().smallFontId);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpLineHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpLineHeight},
                     tr(STR_HOLD_CONFIRM_TO_DELETE));
  }

  // rowLabels/rowSubtitles/rowItems are built whenever `bookmarks` changes (see
  // rebuildRowItems()) and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press deletes (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  // Snippets in the small font so more of a quote fits on the line, and bold so
  // the teaser reads ahead of the chapter/% subtitle. The bold also doubles as
  // the caller-owned marker: an all-default smallText fails textStyleUnset and
  // Screen::list() would substitute bodyText back (FONT_SLOT_SMALL is 0).
  fui::TextStyle label = screen.theme().smallText;
  label.bold = true;
  props.labelText = label;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void EpubReaderBookmarksActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_BOOKMARKS));
}

void EpubReaderBookmarksActivity::drawFooter() {
  const auto confirmLabel = bookmarks.empty() ? "" : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void EpubReaderBookmarksActivity::render(RenderLock&& lock) {
  // The popup draws over the framebuffer the list already left behind (no
  // clear) and owns the frame when it is up, so the base skeleton is skipped
  // entirely rather than repainted underneath it — one panel update, not two.
  if (confirmPopup.processRender(renderer, mappedInput)) return;

  UiListActivity::render(std::move(lock));
}
