#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <SdDebugLog.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <iterator>
#include <memory>

#include "CrossPointSettings.h"
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

// Allowed shelf thumbnail heights, largest first. A LADDER rather than a height
// computed from the cell: an exactly-fitted height would mint a new
// thumb_<H>.bmp on every theme, rotation and column change, and each of those
// pays a full decode pass over every book. Snapping to this fixed set keeps the
// possible sizes countable while still letting the layout adapt -- the content
// band is NOT predictable from the panel alone (Lyra's headerHeight is 84 where
// Base's is 45, and the theme's verticalSpacing comes off the top too), so a
// pair of hardcoded heights renders 3x2 on some themes and 3x3 on others.
// Epub::generateThumbBmp fixes the aspect at height * 0.6. There is no caption
// band under the tiles, so the whole cell is cover.
// Stepped at 10px through the band the shelf actually lands in (a 480-wide
// portrait panel fits 200-380 here): the coarse 30-40px steps this replaced
// quantised so hard that freeing the button-hint strip bought no growth at all
// -- 3x3 cleared the 230 rung by 2px and fell back to 210.
constexpr int SHELF_COVER_LADDER[] = {380, 370, 360, 350, 340, 330, 320, 310, 300, 290, 280, 270,
                                      260, 250, 240, 230, 220, 210, 200, 190, 180, 165, 150};

// Selection ring geometry. The stroke is drawn INSIDE its rect
// (GfxRenderer::drawRect), and coverGrid builds that rect as the cover grown by
// selectedCoverFrameGap on each side -- so the ring's innermost pixel lands
// SHELF_RING_GAP - SHELF_RING_WIDTH away from the cover. The gap must therefore
// exceed the stroke width, or the ring sits directly on the cover edge and
// vanishes against an all-black thumbnail. SHELF_RING_PAD is that surviving
// white moat.
constexpr int16_t SHELF_RING_WIDTH = 3;
constexpr int16_t SHELF_RING_PAD = 3;
constexpr int16_t SHELF_RING_GAP = SHELF_RING_PAD + SHELF_RING_WIDTH;
// The ring grows outward from the cover, so the cell must reserve the whole gap
// or the stroke is clipped. Not gap + width: the stroke is drawn inward from
// the frame rect and is already inside the gap.
constexpr int16_t SHELF_CELL_INSET = SHELF_RING_GAP;

// Gap to hand coverGrid so that all `count + 1` white channels along one axis
// -- the two outer margins and the `count - 1` between covers -- come out the
// same width.
//
// The channel is `slot` wide. An interior one is built from the cell inset on
// each of the two neighbouring cells plus the grid gap; an edge one from a
// single inset plus the outer margin. Solving both for the same slot gives
// gap = slot - 2 * inset, and centring the grid rect then yields the matching
// margin = slot - inset for free.
//
// Zero when the covers already fill the axis, which reproduces the old
// flush-packed layout rather than overlapping the cells.
int16_t evenGap(const int available, const int coverExtent, const int count) {
  if (count <= 0) return 0;
  const int slot = (available - count * coverExtent) / (count + 1);
  const int gap = slot - 2 * SHELF_CELL_INSET;
  return gap > 0 ? static_cast<int16_t>(gap) : 0;
}
}  // namespace

bool RecentBooksActivity::isShelf() const { return SETTINGS.recentBooksView != CrossPointSettings::RECENT_VIEW_LIST; }

int RecentBooksActivity::shelfColumns() const {
  return SETTINGS.recentBooksView == CrossPointSettings::RECENT_VIEW_SHELF_2COL ? 2 : 3;
}

// Largest ladder height that still lets `columns` rows of covers fit the
// measured band -- so "Bookshelf 3x3" really is 3x3, on every theme and both
// panels, rather than silently degrading to 3x2. Falls through to the ladder
// floor when even that will not fit (landscape), where the grid pages instead
// of shrinking the covers to stamps.
//
// Measured against zero inter-row gap: whatever the chosen height leaves over
// becomes the gap, distributed by buildShelf(). Reserving a gap here instead
// would come straight off the cover.
int RecentBooksActivity::pickShelfCoverHeight(const int bodyHeight, const int cellWidth, const int columns) {
  const int rows = columns;  // 2 columns -> 2 rows, 3 columns -> 3 rows
  const int innerWidth = cellWidth - 2 * SHELF_CELL_INSET;
  for (const int height : SHELF_COVER_LADDER) {
    const int needed = rows * (height + 2 * SHELF_CELL_INSET);
    if (needed <= bodyHeight && height * 3 / 5 <= innerWidth) return height;
  }
  return SHELF_COVER_LADDER[std::size(SHELF_COVER_LADDER) - 1];
}

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
  // Header count. Empty stays bare -- the body already says "no recent books",
  // so a "(0)" in the title only repeats it.
  if (recentBooks.empty()) {
    snprintf(headerTitleBuf, sizeof(headerTitleBuf), "%s", tr(STR_MENU_RECENT_BOOKS));
  } else {
    snprintf(headerTitleBuf, sizeof(headerTitleBuf), "%s (%d)", tr(STR_MENU_RECENT_BOOKS),
             static_cast<int>(recentBooks.size()));
  }

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

  // Shelf mode prewarms nothing here. The resident glyph table is exactly what
  // would pin the ~26.6KB contiguous block the cover decoder needs, and the
  // cover pass runs before any of these labels are worth caching. The single
  // title prewarm it does want is issued from loadShelfCovers() once the decode
  // pass is done and the heap has settled.
  if (isShelf()) return;

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

void RecentBooksActivity::stepShelfSelection(const int delta) {
  const int count = listCount();
  if (count <= 0) return;
  int target = nav.selected + delta;
  if (target < 0) target = 0;
  if (target >= count) target = count - 1;
  if (target != nav.selected) moveSelectionTo(target);
}

void RecentBooksActivity::onEnter() {
  UiListActivity::onEnter();

  app.on(ACTION_HEADER, &RecentBooksActivity::headerActionTrampoline, this);

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
  // The picker selects on a Confirm PRESS and dismisses on a Back PRESS; this
  // screen opens a book on the Confirm RELEASE and goes Home on the Back
  // RELEASE. Swallow whichever release ends the press that closed the popup,
  // or picking "Bookshelf 3x3" also opens the highlighted book.
  if (swallowReleaseAfterPopup) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      swallowReleaseAfterPopup = false;
    }
    return true;
  }

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
  // Shelf mode retargets this pair: Left/Right walk the cursor along the row
  // (side Up/Down jump a whole row), which is the only mapping that reads as
  // 2-D. Reordering has no button on the shelf as a result — switch to List to
  // reorder.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (isShelf()) {
      stepShelfSelection(-1);
    } else {
      moveSelectedUp();
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (isShelf()) {
      stepShelfSelection(1);
    } else {
      moveSelectedDown();
    }
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

  // Back hold opens the List / Bookshelf picker. wasLongPressed (not a
  // hand-rolled isPressed + getHeldTime pair) because it also marks the
  // eventual Back release to be swallowed — otherwise that release falls
  // through to the short-press goHome handler below.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Back, LONG_PRESS_MS)) {
    showViewPicker();
    return true;
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
      // Shelf: down a whole row. List: the next row.
      if (isShelf()) {
        stepShelfSelection(shelfColumns());
      } else {
        moveSelectionTo(ButtonNavigator::nextIndex(nav.selected, listSize));
      }
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
      if (isShelf()) {
        stepShelfSelection(-shelfColumns());
      } else {
        moveSelectionTo(ButtonNavigator::previousIndex(nav.selected, listSize));
      }
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

void RecentBooksActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  UiListActivity::render(std::move(lock));

  // Cover generation runs AFTER the first paint, so the shelf appears
  // immediately with empty frames and fills in behind the progress popup,
  // rather than stalling on up to ten decodes before anything is on screen.
  if (isShelf() && !recentBooks.empty() && !shelfCoversLoaded && !shelfCoversLoading) {
    shelfCoversLoading = true;
    loadShelfCovers();
  }
}

// The one place a view change is applied. Both entry points -- the Back-hold
// picker and the header tap -- come through here, because the ordering below is
// load-bearing and a second copy would drift out of step with it.
void RecentBooksActivity::applyView(const int view) {
  if (view < 0 || view >= CrossPointSettings::RECENT_VIEW_COUNT) return;
  if (SETTINGS.recentBooksView == static_cast<uint8_t>(view)) return;
  {
    // The published interaction table indexes the OLD view's hit rects; a tap
    // arriving before the next render would activate by them. Same reason
    // moveSelectedUp closes it.
    RenderLock renderLock(*this);
    SETTINGS.recentBooksView = static_cast<uint8_t>(view);
    closeRouting();
    // The new view wants its own cover pass (or none at all), and its own
    // glyph prewarm.
    shelfCoversLoaded = false;
    shelfCoversLoading = false;
    // rowItems' label pointers are what the render task dereferences, and the
    // prewarm inside depends on the new view -- both must move under the lock,
    // like moveSelectedUp's reload does.
    rebuildRowItems();
    nav.follow(listCount());
    SETTINGS.saveToFile();
  }
  requestUpdate(true);
}

// List / Bookshelf 2x2 / Bookshelf 3x3. Opened by a Back hold; the same three
// values are also on the Settings > Library & Storage row.
void RecentBooksActivity::showViewPicker() {
  static constexpr StrId OPTIONS[] = {StrId::STR_VIEW_LIST, StrId::STR_VIEW_SHELF_2, StrId::STR_VIEW_SHELF_3};
  optionPopup.show(StrId::STR_RECENT_BOOKS_VIEW, OPTIONS, CrossPointSettings::RECENT_VIEW_COUNT,
                   SETTINGS.recentBooksView, [this](const int idx) { applyView(idx); });
  requestUpdate();
}

// Header tap: step to the next view and wrap. Cycling rather than opening the
// picker keeps the switch to one tap, which matters on a panel where every
// change costs a full refresh.
void RecentBooksActivity::headerActionTrampoline(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<RecentBooksActivity*>(user);
  // The flash is keyed to the header rect, which the next layout may not
  // publish; clear it so it cannot gray an unrelated element after the switch.
  self->app.clearTapFlash();
  self->applyView((SETTINGS.recentBooksView + 1) % CrossPointSettings::RECENT_VIEW_COUNT);
}

bool RecentBooksActivity::handleCustomInput() {
  if (optionPopup.isActive()) {
    optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    // Closed this pass: the button that closed it is still down, and its
    // release must not reach handleButtons(). See swallowReleaseAfterPopup.
    if (!optionPopup.isActive()) swallowReleaseAfterPopup = true;
    return true;
  }

  // Shelf paging. The base tail scrolls nav.top on a swipe, but the shelf
  // derives its viewport from the SELECTION, so nav.top moves nothing there --
  // page the selection instead. Guarded by isShelf() so list mode never sees
  // this consuming read of wasSwipe().
  if (!isShelf() || recentBooks.empty()) return false;
  const auto swipe = mappedInput.wasSwipe();
  if (swipe != MappedInputManager::SwipeDir::Up && swipe != MappedInputManager::SwipeDir::Down) return false;

  const int count = listCount();
  int target;
  {
    RenderLock lock(*this);  // shelfPageItems is written by the render task
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? shelfPageItems : -shelfPageItems;
    target = nav.selected + delta;
  }
  if (target < 0) target = 0;
  if (target >= count) target = count - 1;
  if (target != nav.selected) moveSelectionTo(target);
  return true;
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

// Mirrors HomeActivity::loadRecentCovers. Runs from the render tail, on the
// render task, after the shelf has already painted once with empty frames.
void RecentBooksActivity::loadShelfCovers() {
  const int coverHeight = shelfCoverHeight();
  bool showingLoading = false;
  Rect popupRect;
  int generated = 0;

  SdDebugLog::log("RBA", "shelf cover pass h=%d books=%u free=%u largest=%u", coverHeight,
                  static_cast<unsigned>(recentBooks.size()), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    progress++;
    if (book.coverBmpPath.empty()) continue;
    if (Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight).c_str())) continue;

    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    GUI.fillPopupProgress(renderer, popupRect, 10 + progress * 90 / static_cast<int>(recentBooks.size()));

    // The loan hands the 48KB framebuffer to the decoder as build scratch. The
    // JPEG path wants a ~26.6KB contiguous block, which a fragmented UI heap
    // cannot supply on its own -- this is the only reason generation succeeds.
    // Legal here and nowhere else on this screen: the progress popup above is
    // already flushed to the panel, and no frame build is in progress.
    // Nesting-safe: an already-lent framebuffer yields an inert loan.
    bool ok = false;
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      // Metadata only -- generateThumbBmp needs the cache loaded, nothing else.
      epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true);
      GfxRenderer::FrameBufferLoan loan(renderer);
      ok = epub.generateThumbBmp(coverHeight);
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        GfxRenderer::FrameBufferLoan loan(renderer);
        ok = xtc.generateThumbBmp(coverHeight);
      }
    }
    if (ok) generated++;
    // Deliberately NOT clearing book.coverBmpPath on failure: generation can
    // fail transiently under fragmentation, and clearing it would blank the
    // cover until the book is reopened. The painter falls back to the empty
    // frame, and a later entry (fresher heap) retries.
    SdDebugLog::log("RBA", "shelf thumb %s free=%u largest=%u path=%s", ok ? "ok" : "FAILED",
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)), book.path.c_str());
  }

  // Now that the decoder is done with the heap, cache the glyphs the tile
  // labels need. One line per tile, one style -- not the two-style pair list
  // mode prewarms. See the note in rebuildRowItems().
  renderer.prewarmFallbackText(
      uiScaleSpec().smallFontId,
      [](const void* ctx, uint32_t i) -> const char* {
        return (*static_cast<const std::vector<RecentBook>*>(ctx))[i].title.c_str();
      },
      &recentBooks, static_cast<uint32_t>(recentBooks.size()));

  shelfCoversLoaded = true;
  shelfCoversLoading = false;
  if (generated > 0 || showingLoading) requestUpdate();
}

fui::CoverGridItem RecentBooksActivity::shelfItemProvider(const uint16_t index, void* userData) {
  auto* self = static_cast<RecentBooksActivity*>(userData);
  fui::CoverGridItem item;
  if (index >= self->recentBooks.size()) return item;
  // Points straight into recentBooks; the render lock keeps that vector alive
  // for the whole build, the same contract rowItems has in list mode.
  item.title = self->recentBooks[index].title.c_str();
  item.actionValue = static_cast<int16_t>(index);
  return item;
}

// Streams one cached thumbnail into its cell. NEVER generates: generation needs
// a GfxRenderer::FrameBufferLoan, which cannot be taken while a frame is being
// built. A book whose thumb is missing gets the empty frame and its title.
bool RecentBooksActivity::shelfCoverPainter(fui::DrawTarget&, const fui::Rect rect, const fui::CoverGridItem& item,
                                            const uint16_t index, void* userData) {
  auto* self = static_cast<RecentBooksActivity*>(userData);
  const GfxRenderer& renderer = self->renderer;
  // drawBitmap composites dark-only (white never overwrites), so clear first or
  // a previous pass's ink ghosts through the light areas of the cover.
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  if (index < self->recentBooks.size() && !self->recentBooks[index].coverBmpPath.empty()) {
    const std::string thumbPath =
        UITheme::getCoverThumbPath(self->recentBooks[index].coverBmpPath, self->shelfCoverHeight());
    HalFile file;
    if (Storage.openFileForRead("RBA", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        // Crop, never scale: downscaling a pre-dithered 1-bit bitmap darkens it.
        // A negative crop (cell larger than the cover on that axis) is clamped
        // to no-crop by drawBitmap. Same treatment as VegaTheme::drawCoverTile.
        const float cropX = 1.0f - static_cast<float>(rect.width) / static_cast<float>(bitmap.getWidth());
        const float cropY = 1.0f - static_cast<float>(rect.height) / static_cast<float>(bitmap.getHeight());
        // A cover narrower than the cell would sit flush-left; centre it.
        int drawX = rect.x;
        if (bitmap.getWidth() < rect.width) drawX = rect.x + (rect.width - bitmap.getWidth()) / 2;
        renderer.drawBitmap(bitmap, drawX, rect.y, rect.width, rect.height, cropX, cropY);
        return true;
      }
    }
  }

  // Coverless (or not generated yet): an empty frame with the title wrapped
  // inside it. With no caption band under the tiles, this is the only thing
  // identifying the book — the same treatment VegaTheme::drawCoverTile gives a
  // coverless hero. wrappedText ellipsizes UTF-8-safely on overflow.
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  if (item.title != nullptr && item.title[0] != '\0') {
    constexpr int kInset = 4;
    const int fontId = uiScaleSpec().smallFontId;
    const int innerW = rect.width - 2 * kInset;
    const int lineH = renderer.getLineHeight(fontId);
    const int maxLines = std::max(1, (rect.height - 2 * kInset) / lineH);
    const auto lines = renderer.wrappedText(fontId, item.title, innerW, maxLines);
    int lineY = rect.y + (rect.height - static_cast<int>(lines.size()) * lineH) / 2;
    for (const auto& line : lines) {
      const int lineW = renderer.getTextWidth(fontId, line.c_str());
      renderer.drawText(fontId, rect.x + (rect.width - lineW) / 2, lineY, line.c_str(), true);
      lineY += lineH;
    }
  }
  return true;
}

void RecentBooksActivity::buildShelf(UiScreen& screen) {
  const fui::Rect body = screen.body();
  const int columns = shelfColumns();
  fui::CoverGridProps props;

  // Width coverGrid takes off the right for its scroll track once the shelf
  // overflows. Held back from the cover sizing unconditionally: whether it is
  // actually claimed depends on the row count, which is not known until the
  // cover height has been chosen, and a cover sized over it would be clipped.
  const auto scrollGutter = static_cast<int16_t>(props.scrollIndicatorWidth + props.scrollIndicatorGap);

  // Cover box, then the row that contains it. rowHeight follows the cover
  // constant rather than dividing the band by the column count: that is what
  // keeps cells full-size and makes the shelf PAGE when the band is short
  // (landscape) instead of squeezing every tile into an unreadable stamp.
  // Both axes lose SHELF_CELL_INSET on each side to the selection ring.
  constexpr int16_t insetBoth = 2 * SHELF_CELL_INSET;
  // Sized against the whole band bar the gutter: the inter-column gap is
  // whatever the chosen cover leaves over, so it cannot be subtracted before
  // the cover is known.
  const auto cellWidth = static_cast<int16_t>((body.width - scrollGutter) / columns);
  // Measured, not assumed: the band depends on the theme's header/spacing and
  // on the bezel safe area, so it cannot be predicted from the panel size.
  const int picked = pickShelfCoverHeight(body.height, cellWidth, columns);
  if (picked != shelfCoverH) {
    // A different height is a different thumb_<H>.bmp; the cached set for the
    // old height is no longer what the painter asks for. Same task as the
    // render tail that reads this, so no lock is needed.
    shelfCoverH = picked;
    shelfCoversLoaded = false;
  }
  auto coverH = static_cast<int16_t>(shelfCoverH);
  auto rowHeight = static_cast<int16_t>(coverH + insetBoth);
  // A theme with unusually tall chrome could leave a band shorter than one row,
  // which would draw nothing at all. Crop the cover down so one row always fits.
  if (rowHeight > body.height && body.height > insetBoth) {
    coverH = static_cast<int16_t>(body.height - insetBoth);
    rowHeight = body.height;
  }
  // generateThumbBmp fixes the aspect at height * 0.6; never let the cover box
  // exceed its cell, or neighbouring tiles would overlap.
  auto coverW = static_cast<int16_t>(coverH * 3 / 5);
  if (coverW > cellWidth - insetBoth) coverW = static_cast<int16_t>(cellWidth - insetBoth);

  props.itemProvider = &RecentBooksActivity::shelfItemProvider;
  props.itemProviderUserData = this;
  props.count = static_cast<uint16_t>(recentBooks.size());
  props.columns = static_cast<uint8_t>(columns);
  props.coverSize = fui::Size{coverW, coverH};
  props.rowHeight = rowHeight;
  props.cellInset = fui::Insets{SHELF_CELL_INSET, SHELF_CELL_INSET, SHELF_CELL_INSET, SHELF_CELL_INSET};
  // No caption band: the tile is all cover. A book with no usable cover is
  // identified by its title drawn inside the empty frame (see the painter).
  props.labelHeight = 0;
  props.action = ACTION_ROW;
  // Tap opens; long-press prompts removal — the same pair the list rows carry,
  // so the gesture means the same thing in both views.
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  // Ring the cover rather than washing the whole cell: on e-ink a filled
  // selection block behind a 1-bit cover erases the cover.
  props.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  props.selectedCoverFrameGap = SHELF_RING_GAP;
  props.selectedCoverFrameWidth = SHELF_RING_WIDTH;
  props.selectedIndex = static_cast<int16_t>(nav.selected);
  // Every cell paints solid white first, selected or not: that white moat
  // between cover and ring is what keeps the selection readable on an
  // all-black thumbnail. Selected cells must NOT take a gray wash — it would
  // sit behind a 1-bit cover and erase it.
  fui::StyleSet cellStyles;
  cellStyles.explicitlySet = true;
  cellStyles.normal.background = fui::Paint::solid(fui::Color::White);
  cellStyles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  cellStyles.selected = cellStyles.normal;
  cellStyles.selected.border = fui::Paint::solid(fui::Color::Black);
  cellStyles.focused = cellStyles.selected;
  cellStyles.active = cellStyles.selected;
  props.cellStyles = cellStyles;

  // Spread the leftover band evenly instead of banking it all as an outer
  // margin, which lumped the covers together in the middle of the screen.
  //
  // Treat the row as `n` covers separated by `n + 1` equal slots -- one at each
  // edge, one between each pair -- so every visible white channel is the same
  // width. Each slot already contains the cell inset the selection ring needs:
  // an interior slot carries two of them (one per neighbouring cell) and an
  // edge slot one, so the gap handed to coverGrid is the slot minus the inset
  // it already spends, and the outer margin falls out of centring the result.
  //
  // coverGrid centres each cover inside its cell (cover-grid.h: coverRect.x =
  // content.x + (content.width - coverSize.width) / 2), so a cell wider than
  // coverW would put its own surplus BETWEEN the covers on top of the gap and
  // desynchronise the two axes. Sizing the grid rect to exactly
  // columns * tightCell + gaps keeps cellW == tightCell.
  //
  // Order matters: coverH was chosen against the FULL-width cellWidth above, so
  // the surplus is known only now.
  const auto tightCell = static_cast<int16_t>(coverW + insetBoth);

  // Rows first: the row count decides whether the shelf overflows, and that
  // decides whether coverGrid claims a scrollbar gutter off the right. Not
  // always `columns` rows -- in landscape the ladder bottoms out and fewer fit,
  // and distributing over a row count the band cannot hold would push the last
  // row off the bottom.
  const int fitRows = rowHeight > 0 ? std::max(1, body.height / rowHeight) : 1;
  props.rowGap = evenGap(body.height, coverH, fitRows);

  // coverGrid takes its scroll track out of the rect it was handed, which would
  // shave the last column and pull every cover off the pitch computed here.
  // Reserve it instead, so the covers keep an exact cell and the track sits in
  // its own gutter.
  const bool overflows = static_cast<int>(recentBooks.size()) > fitRows * columns;
  const auto gutter = static_cast<int16_t>(overflows ? scrollGutter : 0);
  props.gap = evenGap(body.width - gutter, coverW, columns);

  // Exactly the cells laid out above, then centred. Sizing the rect to the
  // content (rather than handing over the whole band) is what makes
  // coverGridVisibleCells return fitRows * columns exactly and leaves the
  // residue as a matching outer margin.
  fui::Rect grid = body;
  const auto gridWidth = static_cast<int16_t>(columns * tightCell + (columns - 1) * props.gap);
  const auto claimedWidth = static_cast<int16_t>(gridWidth + gutter);
  if (gridWidth > 0 && claimedWidth < body.width) {
    grid.x = static_cast<int16_t>(body.x + (body.width - claimedWidth) / 2);
    grid.width = claimedWidth;
  }
  const auto gridHeight = static_cast<int16_t>(fitRows * rowHeight + (fitRows - 1) * props.rowGap);
  if (gridHeight > 0 && gridHeight < body.height) {
    grid.y = static_cast<int16_t>(body.y + (body.height - gridHeight) / 2);
    grid.height = gridHeight;
  }

  // The shelf owns its own viewport: syncListViewport() is row-height based and
  // would fight this. topIndex is derived from the selection, so stepping past
  // the last visible cell pages the shelf for free.
  const uint16_t pageItems = fui::coverGridVisibleCells(grid, props.columns, props.rowHeight, props.rowGap);
  shelfPageItems = pageItems > 0 ? static_cast<int>(pageItems) : 1;
  props.topIndex = fui::coverGridTopIndexFor(static_cast<uint16_t>(nav.selected), props.count, props.columns,
                                             pageItems > 0 ? pageItems : props.columns);

  props.coverPainter = &RecentBooksActivity::shelfCoverPainter;
  props.coverPainterUserData = this;

  fui::coverGrid(screen.frame(), grid, props);
}

void RecentBooksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints -- except on
  // the shelf, which draws no hints (drawFooter) and takes the strip as cover
  // height instead. Already 0 on a touch board: UITheme::getMetrics() zeroes
  // buttonHintsHeight whenever the panel has touch.
  const auto bottomReserve = static_cast<int16_t>(isShelf() ? 0 : metrics.buttonHintsHeight);
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0, bottomReserve, 0});
  // The shelf skips the theme's leading spacer too: the covers are their own
  // separation, and on a tall-header theme those pixels are a whole ladder rung.
  if (!isShelf()) screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Tap the title to cycle List -> 2x2 -> 3x3. drawChrome() paints the header
  // straight to the renderer, outside the frame, so the band carries no hit
  // rect of its own -- this publishes one over exactly the rect drawChrome()
  // draws into. Registered before the empty-list return so the view is still
  // switchable with no books on the shelf.
  //
  // Touch only: a button board reaches the picker through the Back hold, and an
  // unreachable rect would just spend one of the frame's interaction slots.
  if (mappedInput.hasTouch()) {
    screen.frame().hit(
        fui::Rect{0, static_cast<int16_t>(metrics.topPadding), static_cast<int16_t>(renderer.getScreenWidth()),
                  static_cast<int16_t>(metrics.headerHeight)},
        ACTION_HEADER, 0, fui::InputTouch);
  }

  if (recentBooks.empty()) {
    screen.centeredText(tr(STR_NO_RECENT_BOOKS), screen.theme().smallText);
    return;
  }

  if (isShelf()) {
    buildShelf(screen);
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
  // The shelf trades the whole hint strip for cover height; buildScreen() stops
  // reserving the band to match. The gestures are unchanged -- only the on-screen
  // reminder goes. List view keeps its hints: it has the room, and they are the
  // only place the Left/Right reorder gesture is advertised.
  if (isShelf()) return;

  // No rows: blank the row-action hints, same as FileBrowserActivity.
  const bool empty = recentBooks.empty();
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), empty ? "" : tr(STR_OPEN), empty ? "" : tr(STR_DIR_UP),
                                            empty ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
