#include "UiAppHost.h"

#include <BoardConfig.h>

#include "MappedInputManager.h"
#include "UiAppHelpers.h"
#include "activities/RenderLock.h"
#include "components/icons/listIcons.h"

namespace fui = freeink::ui;

UiAppHost::UiAppHost(const GfxRenderer& renderer)
    : uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void UiAppHost::resetUi() {
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
}

void UiAppHost::renderUi() {
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
}

UiAppHost::TouchRoute UiAppHost::routeTouch(const MappedInputManager& input, const bool withLongPress,
                                            const bool routeHeld) {
  TouchRoute result;  // named apart from route() — cppcheck flags the shadow
  if (!uiReady) return result;
  result.snap = touchSnapshotFrom(input, withLongPress);
  if (!result.snap.touchPressed && !result.snap.touchReleased && !(routeHeld && result.snap.touchHeld)) {
    return result;
  }
  result.routed = true;
  onBeforeRoute(result.snap);
  result.event = app.route(result.snap);
  return result;
}

fui::ActionEvent UiAppHost::route(const fui::InputSnapshot& snap) {
  if (!uiReady) return {};
  return app.route(snap);
}

UiAppHost::SwipeDelete UiAppHost::handleSwipeDelete(const MappedInputManager& input, const fui::ActionId rowAction,
                                                    const bool withLongPress, bool (*canDelete)(void*, int),
                                                    void* ctx) {
  SwipeDelete result;
#if FREEINK_CAP_TOUCH
  if (!input.hasTouch()) return result;
  const int openRow = revealedRow.load();
  if (openRow >= 0) {
    if (input.wasReleased(MappedInputManager::Button::Back) || input.wasReleased(MappedInputManager::Button::Confirm)) {
      closeSwipeDelete();
      result.kind = SwipeDelete::Kind::Consumed;
      return result;
    }
    int x = 0;
    int y = 0;
    if (input.wasScreenTapped(x, y)) {
      bool onDelete = false;
      {
        RenderLock lock;
        fui::Interaction hit;
        onDelete = routingReady() &&
                   app.hitPublished(static_cast<int16_t>(x), static_cast<int16_t>(y), ACTION_SWIPE_DELETE, hit) &&
                   hit.value == openRow;
        // Cancel the press the app is tracking, so the row under the finger does not
        // activate on this tap.
        if (routingReady()) app.route(fui::InputSnapshot{.touchReleased = true, .touchX = -1, .touchY = -1});
      }
      app.clearTapFlash();
      closeSwipeDelete();
      result.kind = onDelete ? SwipeDelete::Kind::Delete : SwipeDelete::Kind::Consumed;
      result.row = onDelete ? openRow : -1;
      return result;
    }
  }

  int sx = 0;
  int sy = 0;
  const auto swipe = input.wasSwipe(&sx, &sy);
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    closeSwipeDelete();  // the list scrolls; its own repaint drops the trash
    return result;
  }
  if (swipe == MappedInputManager::SwipeDir::Right && openRow >= 0) {
    routeTouch(input, withLongPress);  // the gesture's release must not reach the rows
    closeSwipeDelete();
    result.kind = SwipeDelete::Kind::Consumed;
    return result;
  }
  if (swipe != MappedInputManager::SwipeDir::Left) return result;

  fui::Interaction hit;
  {
    RenderLock lock;
    if (!routingReady() || !app.hitPublished(static_cast<int16_t>(sx), static_cast<int16_t>(sy), rowAction, hit) ||
        !canDelete(ctx, hit.value)) {
      return result;
    }
  }
  routeTouch(input, withLongPress);
  revealedRow.store(hit.value);
  result.kind = SwipeDelete::Kind::Consumed;
#else
  (void)input;
  (void)rowAction;
  (void)withLongPress;
  (void)canDelete;
  (void)ctx;
#endif
  return result;
}

void UiAppHost::configureSwipeDelete(const UiScreen& screen, fui::ListProps& props) {
#if FREEINK_CAP_TOUCH
  const int row = revealedRow.load();
  if (row < 0 || row >= props.count) return;
  const auto& theme = screen.theme();
  const int16_t rowHeight = props.rowHeight > 0 ? props.rowHeight : theme.rowHeight;
  swipeReveal.index = static_cast<int16_t>(row);
  swipeReveal.action = ACTION_SWIPE_DELETE;
  swipeReveal.icon = fui::bitmapFromIcon(icon_trash_2_24);
  swipeReveal.width = static_cast<int16_t>(rowHeight + theme.spaceSm * 2);
  props.reveal = &swipeReveal;
#else
  (void)screen;
  (void)props;
#endif
}
