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
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
// Hold threshold for the long-press "remove from list" action (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
}  // namespace

RecentBooksActivity::RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("RecentBooks", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

bool RecentBooksActivity::usesBodyLabel() const { return mappedInput.hasTouch(); }

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

  // One SD pass for every CJK title/author on the screen; repaints then hit the resident
  // tables instead of re-reading per string. Authors draw bold and titles regular (see
  // buildScreen), so they need separate per-style prewarms, and the title line follows the
  // same font branch the rows are drawn with. Getter form: no concatenated copy, whose
  // bare-new growth is what abort()s under heap pressure. See GfxRenderer::prewarmFallbackText().
  const auto count = static_cast<uint32_t>(recentBooks.size());
  renderer.prewarmFallbackText(
      usesBodyLabel() ? uiScaleSpec().bodyFontId : uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<RecentBook>*>(ctx))[i].title.c_str();
      },
      &recentBooks, count);
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<RecentBook>*>(ctx))[i].author.c_str();
      },
      &recentBooks, count, EpdFontFamily::BOLD);
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
    // The rows swapped under the published interaction table, so a tap arriving
    // before the next render would activate by the pre-move index; and without
    // follow() the moved row can walk off the top of the viewport, which just
    // looks like the book disappearing.
    closeRouting();
    nav.follow(listCount());
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
    closeRouting();
    nav.follow(listCount());
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
  bool consumed = false;

  // Cursor moves on the Up/Down side buttons only — Left/Right are reserved for
  // reordering above, so they are deliberately excluded from navigation here.
  // Single-step only (no continuous page-jump): holding side Up/Down is
  // reserved for the display-orientation-cycle gesture instead.
  // moveSelectionTo (not a bare nav.selected write): it takes the render lock
  // and pulls the viewport to the new selection, which a raw write does not.
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Down)) {
    case ReaderUtils::SideNavAction::STEP:
      moveSelectionTo(ButtonNavigator::nextIndex(nav.selected, listSize));
      consumed = true;
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, -1);
      requestUpdate();
      consumed = true;
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
  switch (ReaderUtils::resolveSideNavAction(mappedInput, MappedInputManager::Button::Up)) {
    case ReaderUtils::SideNavAction::STEP:
      moveSelectionTo(ButtonNavigator::previousIndex(nav.selected, listSize));
      consumed = true;
      break;
    case ReaderUtils::SideNavAction::ROTATE:
      ReaderUtils::cycleDisplayOrientation(renderer, 1);
      requestUpdate();
      consumed = true;
      break;
    case ReaderUtils::SideNavAction::NONE:
      break;
  }
  // false when nothing fired, so touch and swipe still reach the rest of loop().
  return consumed;
}

// This screen owns all four navigable buttons itself (handleButtons): side
// Up/Down step or rotate, front Left/Right reorder. The base tail would claim
// the same four — its next/previous sets are {Down, Right} / {Up, Left} — and
// on a different edge: it steps on RELEASE where resolveSideNavAction's default
// branch steps on PRESS, so every side tap moved the cursor twice, and holding
// Left/Right page-jumped the cursor before the release reordered whatever book
// had scrolled under it.
void RecentBooksActivity::navigateButtons() {}

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
  // Book rows read exactly like the OPDS browser's (buildBrowsingScreen): the
  // title on the first line, the author under it, and only the SHORT line bold
  // — bold glyphs are wider, so the title stays regular and fits more
  // characters before it ellipsizes. Both lines keep the default maxLines, so
  // a row is exactly two lines, never three (a wrapped title would collide
  // with the author anyway: the label band is one line tall).
  props.labelText = usesBodyLabel() ? screen.theme().bodyText : screen.theme().smallText;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.bold = true;
  // Both styles are final from here: textStylesExplicit stops Screen::list()
  // from substituting the larger bodyText back over an all-default smallText,
  // which it cannot tell apart from "caller left this unset".
  props.textStylesExplicit = true;
  // Minimum air around the two-line block for themes whose row height spares
  // none of its own (see ListProps::subtitleRowPadding).
  props.subtitleRowPadding = screen.theme().spaceMd;
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
