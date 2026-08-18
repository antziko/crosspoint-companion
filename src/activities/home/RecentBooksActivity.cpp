#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

RecentBooksActivity::RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("RecentBooks", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks = RECENT_BOOKS.getBooks();
  rebuildRowItems();
}

// Derives rowItems from recentBooks. Called whenever recentBooks changes
// (loadRecentBooks(), i.e. load/removal) so buildScreen() reuses the cached
// rows on every repaint instead of rebuilding them per render.
void RecentBooksActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(recentBooks.size());
  for (const auto& book : recentBooks) {
    fui::ListItem item;
    item.label = book.title.c_str();
    if (!book.author.empty()) item.subtitle = book.author.c_str();
    item.icon = listIconFor(UITheme::getFileIcon(book.path), 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

bool RecentBooksActivity::moveSelectedUp() {
  if (!RECENT_BOOKS.moveUp(nav.selected)) {
    return false;
  }
  {
    // loadRecentBooks() refills recentBooks and rebuilds rowItems, whose
    // label/subtitle pointers the render task dereferences; mutate under the
    // render lock (same race as FileBrowserActivity, #3034). Released before
    // requestUpdate().
    RenderLock lock(*this);
    nav.selected--;
    RECENT_BOOKS.saveToFile();
    loadRecentBooks();
  }
  requestUpdate();
  return true;
}

bool RecentBooksActivity::moveSelectedDown() {
  if (!RECENT_BOOKS.moveDown(nav.selected)) {
    return false;
  }
  {
    RenderLock lock(*this);  // see moveSelectedUp()
    nav.selected++;
    RECENT_BOOKS.saveToFile();
    loadRecentBooks();
  }
  requestUpdate();
  return true;
}

void RecentBooksActivity::onEnter() {
  UiListActivity::onEnter();

  // One of the few non-reader screens that follows SETTINGS.displayOrientation
  // (the hold-to-rotate gesture is handled in loop(), see resolveSideNavAction).
  ReaderUtils::applyOrientation(renderer, SETTINGS.displayOrientation);

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned (the other is addBook).
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  loadRecentBooks();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // rowItems' label/subtitle pointers alias recentBooks' strings; drop both.
  rowItems.clear();
  recentBooks.clear();
}

void RecentBooksActivity::activateIndex(const int index) {
  // The interaction table can deliver a row index captured before a removal
  // shrank the list; the next render re-registers the rows.
  if (index < 0 || index >= listCount()) return;
  // Opening the book leaves this screen; a lingering flash would gray an
  // unrelated row when the list next appears.
  app.clearTapFlash();
  LOG_DBG("RBA", "Selected recent book: %s", recentBooks[index].path.c_str());
  onSelectBook(recentBooks[index].path);
}

void RecentBooksActivity::onRowLongPress(const int index) {
  if (index < 0 || index >= listCount()) return;
  // Long-press prompts removal from the list (mirrors the Confirm-button hold).
  app.clearTapFlash();
  promptRemoveBook(recentBooks[index].path, recentBooks[index].title);
}

bool RecentBooksActivity::handleButtons() {
  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return true;
  }

  // Reorder: tap Left = move the selected book up, tap Right = move it down. The
  // cursor is moved with the Up/Down side buttons instead. Tap-based (not hold-based)
  // because the X3 front buttons bounce into a stream of release events when held, so
  // a sustained-press gesture never registers there — only discrete taps are reliable.
  // Intercept before the buttonNavigator block so Left/Right don't also move the cursor.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    moveSelectedUp();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    moveSelectedDown();
    return true;
  }

  // Long-press Confirm on the selected book: prompt to remove it from the list.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && nav.selected < listCount() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    promptRemoveBook(recentBooks[nav.selected].path, recentBooks[nav.selected].title);
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && nav.selected < listCount()) {
      activateIndex(nav.selected);
      return true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return true;
  }

  // LOCAL(feat): side Up/Down are STEP-or-ROTATE, not plain navigation, so the
  // base handler is deliberately not called for them. Holding a side button
  // cycles the display orientation (this is one of the few non-reader screens
  // that follows SETTINGS.displayOrientation); Left/Right are reserved for
  // reordering above. Upstream has neither gesture and just returns false here.
  const int listSize = static_cast<int>(recentBooks.size());

  // Cursor moves on the Up/Down side buttons only — Left/Right are reserved for
  // reordering above, so they are deliberately excluded from navigation here.
  // Single-step only (no continuous page-jump): holding side Up/Down is
  // reserved for the display-orientation-cycle gesture instead.
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Down)) {
    case ReaderUtils::SideNavAction::STEP:
      nav.selected = ButtonNavigator::nextIndex(nav.selected, listSize);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, -1);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Up)) {
    case ReaderUtils::SideNavAction::STEP:
      nav.selected = ButtonNavigator::previousIndex(nav.selected, listSize);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, 1);
      requestUpdate();
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
  return false;
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBA", "Removed from recents: %s", path.c_str());
      {
        // Result handlers run with the render lock released (ActivityManager
        // unlocks before dispatch precisely so a handler can take its own), and
        // loadRecentBooks() invalidates every row pointer. See moveSelectedUp().
        RenderLock lock(*this);
        // The interaction table still indexes the pre-removal rows; stop routing
        // touches against it until the next render republishes.
        closeRouting();
        loadRecentBooks();
        if (recentBooks.empty()) {
          nav.selected = 0;
        } else if (nav.selected >= listCount()) {
          nav.selected = listCount() - 1;
        }
        nav.follow(listCount());
      }
      requestUpdate(true);
    }
  };

  startActivityForResultNoThrow<ConfirmationActivity>(std::move(handler), renderer, mappedInput,
                                                      tr(STR_REMOVE_FROM_RECENTS), title);
}

void RecentBooksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                      static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (recentBooks.empty()) {
    screen.centeredText(tr(STR_NO_RECENT_BOOKS), screen.theme().bodyText);
    return;
  }

  // rowItems is built in loadRecentBooks() (see rebuildRowItems()) and
  // reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  // Tap opens; long-press prompts removal (physical buttons stay in loop()).
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  // Titles in the small font so more of a long title fits on the line; the row
  // height stays on the theme cadence. Bold keeps the title/author hierarchy
  // and doubles as the caller-owned marker: an all-default smallText fails
  // textStyleUnset and Screen::list() would substitute bodyText back
  // (FONT_SLOT_SMALL is 0). No maxLines=2 here: on subtitle rows the label
  // band is one line tall and a wrapped title would collide with the author.
  fui::TextStyle label = screen.theme().smallText;
  label.bold = true;
  props.labelText = label;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void RecentBooksActivity::drawFooter() {
  // No rows: blank the row-action hints, same as FileBrowserActivity.
  const bool empty = recentBooks.empty();
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), empty ? "" : tr(STR_OPEN), empty ? "" : tr(STR_DIR_UP),
                                            empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
