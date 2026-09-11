#include "UiListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "TouchFeedback.h"
#include "components/ListCursor.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
// Minimum air a row that grows for a subtitle keeps around the pair, when the
// row height it was laid out on cannot spare any of its own.
constexpr int SUBTITLE_ROW_PADDING = 8;
}  // namespace

UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const bool wantsTouchLongPress)
    : Activity(name, renderer, mappedInput), UiAppHost(renderer), wantsTouchLongPress(wantsTouchLongPress) {}

void UiListActivity::onEnter() {
  Activity::onEnter();
  activeNav().reset();
  resetUi();
  app.on(ACTION_ROW, &UiListActivity::rowActionTrampoline, this);
  app.setScreen(&UiListActivity::screenTrampoline, this);
  requestUpdate();
}

void UiListActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<UiListActivity*>(user)->buildScreen(screen);
}

void UiListActivity::rowActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<UiListActivity*>(user);
  if (event.value < 0 || event.value >= self->listCount()) return;
  self->onRowAction(event);
}

void UiListActivity::onRowAction(const fui::ActionEvent& event) {
  activeNav().selected = event.value;
  if (event.longPress) {
    onRowLongPress(event.value);
    return;
  }
  activateIndex(event.value);
}

bool UiListActivity::handleButtons() {
  // Act on the RELEASE, but only when the matching press landed in this activity.
  //
  // Acting on the press meant the release outlived the screen: a row that opens a
  // sub-activity handed it the still-pending release, which the sub-activity then read as
  // its own input. The scattered consume/swallow flags around the codebase all work
  // around that. Latching the press instead fixes it at the source and — unlike simply
  // moving to wasReleased — stays correct while other screens still act on press, so this
  // needs no lockstep change. Same shape as OpdsServerListActivity::handleButtons(), which
  // arrived at it independently for its hold-to-duplicate gesture.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressActive = true;
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressActive = true;

  // The latch is cleared before dispatching: onBackButton()/activateIndex() can finish()
  // this activity, and touching members afterwards would be a use-after-free.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    const bool originatedHere = backPressActive;
    backPressActive = false;
    if (originatedHere) onBackButton();
    return true;  // swallow either way — a foreign release must not fall through to touch
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool originatedHere = confirmPressActive;
    confirmPressActive = false;
    if (originatedHere) {
      const int selected = activeNav().selected;
      if (selected >= 0 && selected < listCount()) activateIndex(selected);
    }
    return true;
  }

  return false;
}

bool UiListActivity::routeListTouch() {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let the action trampoline dispatch.
  const auto route = UiAppHost::routeTouch(mappedInput, wantsTouchLongPress);
  // No pressed-state repaint: the render it triggers would drop a slow tap's
  // release inside the uiReady window (tap-to-activate needed two taps), and
  // it costs a second e-ink refresh per tap.
  if (route.routed && app.invalidated()) requestUpdate();
  return static_cast<bool>(route);  // dispatched to the action handler
}

void UiListActivity::moveSelectionTo(const int index) {
  {
    // The render task reads nav mid-build (syncToProps, and now the
    // onListRendered layout feedback writes top/drawnRows back into it), so a
    // press landing during a render would otherwise tear selection/viewport.
    // Released before requestUpdate(): RenderLock is non-recursive.
    RenderLock lock(*this);
    auto& n = activeNav();
    n.selected = index;
    n.follow(listCount());
  }
  requestUpdate();
}

void UiListActivity::loop() {
  if (handleCustomInput()) return;
  if (handleButtons()) return;
  if (routeListTouch()) return;

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    bool moved = false;
    {
      // Same nav-vs-render race as moveSelectionTo: the render task writes
      // top/drawnRows mid-build, so read and mutate under one lock.
      RenderLock lock(*this);
      auto& n = activeNav();
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? n.pageRows() : -n.pageRows();
      moved = n.scrollBy(delta, listCount());
    }
    if (moved) requestUpdate();
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() {
  const int count = listCount();
  auto& n = activeNav();
  buttonNavigator.onNextRelease([this, count, &n] { moveSelectionTo(ButtonNavigator::nextIndex(n.selected, count)); });
  buttonNavigator.onPreviousRelease(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousIndex(n.selected, count)); });
  // Page by the rows the last build actually drew (pageRows), not the
  // fixed-height visibleRows estimate: rows whose label wraps to a second line
  // grow, so the estimate overshoots and the rows between two pages would never
  // be shown. Every screen that sets maxLines = 2 (Settings, Status Bar, Text
  // Settings, Wi-Fi) is affected on the dense X3/X4 row heights, where the
  // two-line label always exceeds the theme row height.
  buttonNavigator.onNextContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::nextPageIndex(n.selected, count, n.pageRows())); });
  buttonNavigator.onPreviousContinuous(
      [this, count, &n] { moveSelectionTo(ButtonNavigator::previousPageIndex(n.selected, count, n.pageRows())); });
}

int16_t UiListActivity::resolveRowHeight(fui::ListProps& props, const bool hasSubtitle) const {
  // The theme's own row height, which is the number BaseTheme::drawList lays
  // legacy lists out on -- so a FreeInkUI list and a legacy one (File Browser,
  // the bookmark lists) read identically on the same screen, on every board.
  // FreeInkUI's theme token is not used: it is sized for a label PLUS a
  // subtitle (lineHeight * 2 + 8), which is a second text line most rows never
  // draw. props.rowHeight must be written explicitly, or screen.list()
  // substitutes that token back instead of taking this value.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto rowHeight = static_cast<int16_t>(hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight);
  props.rowHeight = rowHeight;
  // A row is a MINIMUM, not a clip: list() sizes each row individually, so a row
  // that carries a subtitle in an otherwise single-line list, or whose label
  // wraps, grows past this on its own. Keep such a row off its neighbours.
  if (props.subtitleRowPadding <= 0) props.subtitleRowPadding = SUBTITLE_ROW_PADDING;
  return rowHeight;
}

void UiListActivity::syncListViewport(UiScreen& screen, fui::ListProps& props, const bool hasSubtitle) {
  const int16_t rowHeight = resolveRowHeight(props, hasSubtitle);
  const auto body = screen.body();
  const int16_t rowGap = screen.theme().listRowGap;
  listBand_ = Rect{body.x, body.y, body.width, body.height};
  listRowHeight_ = rowHeight;
  listRowStep_ = rowHeight + rowGap;
  activeNav().syncToProps(body, rowHeight, rowGap, listCount(), props);
  // Withhold the highlight on a list the user has not navigated yet. Applied after
  // syncToProps so the viewport it just computed is untouched -- only what list() DRAWS as
  // selected changes (-1 = no selected row).
  if (ListCursor::suppressed(props.selectedIndex)) props.selectedIndex = -1;
}

void UiListActivity::onBeforeRoute(const fui::InputSnapshot& snap) {
  // Acknowledge the tap on the row the finger landed on, before the dispatch this hook
  // precedes can leave the screen. FreeInkApp's own tap flash already covers a row that
  // STAYS put — it repaints that row with the focused style in the refresh that shows the
  // tap's result, at no extra panel cost — but a row that opens something never gets that
  // repaint, and that is the case this covers.
  if (!snap.touchReleased || snap.longPress) return;
  const auto& listNav = activeNav();
  // Fixed-height rows only. visibleRows is the fixed-height estimate and drawnRows what
  // the build actually laid out; when they disagree a row grew (a wrapped label, a
  // subtitle), the uniform grid below no longer describes what is on screen, and a tint on
  // the wrong band reads as a glitch. Skip the feedback rather than guess at the rect.
  if (listRowStep_ <= 0 || listNav.drawnRows <= 0 || listNav.drawnRows != listNav.visibleRows) return;
  if (snap.touchX < listBand_.x || snap.touchX >= listBand_.x + listBand_.width) return;
  const int offset = snap.touchY - listBand_.y;
  if (offset < 0) return;
  const int row = offset / listRowStep_;
  if (row >= listNav.drawnRows || offset % listRowStep_ >= listRowHeight_) return;  // gap: no row
  if (listNav.top + row >= listCount()) return;
  flashTouchedRow(renderer, Rect{listBand_.x, listBand_.y + row * listRowStep_, listBand_.width, listRowHeight_});
}

void UiListActivity::drawChrome() {
  const char* title = headerTitle();
  if (!title) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title);
}

void UiListActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UiListActivity::renderListFrame(void (*drawFrame)(void*), void* ctx) {
  drawFrame(ctx);
  // Bounded: onListRendered() moves top strictly forward toward the selection
  // each pass, and a viewport starting at the selection always draws it, so
  // this converges well inside the cap. The cap is belt-and-braces against a
  // subclass whose row heights are not stable across builds.
  for (int pass = 0; activeNav().consumeRebuildNeeded() && pass < 8; ++pass) {
    drawFrame(ctx);
  }
}

void UiListActivity::render(RenderLock&&) {
  renderListFrame(
      [](void* ctx) {
        auto* self = static_cast<UiListActivity*>(ctx);
        self->renderer.clearScreen();
        self->drawChrome();
        self->renderUi();
      },
      this);
  drawFooter();
  renderer.displayBuffer();
}
