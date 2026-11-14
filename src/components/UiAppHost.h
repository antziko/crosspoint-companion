#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

class GfxRenderer;
class MappedInputManager;

// Owns the FreeInkApp hosting protocol every FUI screen shares, list-shaped or
// not: the font-bound render target, the app, and the uiReady handshake that
// lets the loop task route touch snapshots against the interaction table the
// render task rebuilds. FreeInkApp keeps the last complete interaction table
// published while it builds the next one, so the gate closes only when the
// old table is semantically invalid (screen reset/explicit close), not around
// an ordinary repaint. The handshake lives here exactly once so no screen
// re-implements (and mis-orders) it.
//
// UiListActivity layers list navigation on top of this; screens that are not a
// single list (sliders, prompts, state machines) inherit or hold this directly
// and keep their own input/loop logic.
class UiAppHost {
 public:
  // One shared instantiation for every FUI screen (the largest of the sizes
  // the screens used to pick individually): FreeInkApp, Screen and Frame are
  // capacity-templated, so per-screen capacities each minted a fresh copy of
  // that code in flash. The wider interaction buffer costs ~300 bytes of RAM
  // per live host, bounded by the activity stack depth.
  using UiApp = freeink::ui::FreeInkApp<24, 6>;
  using UiScreen = UiApp::ScreenType;

  explicit UiAppHost(const GfxRenderer& renderer);

  // Screen-entry reset: close the routing gate and rebind the shared theme
  // tokens (refreshed for the active UITheme + this target's fonts). Call from
  // onEnter/loadOnce before registering actions and the screen fn.
  void resetUi();

  // Re-derives the device context, renders into FreeInkApp's non-published
  // interaction-table generation, then opens uiReady after the first publish.
  // Once open, routing stays available during later renders through the last
  // complete published generation. Call from the render task wherever the app
  // should paint; chrome before, hints after.
  void renderUi();

  // What loop-task routing saw this pass. `routed` is true when the gate was
  // open and the snapshot carried relevant touch input — the invalidated()
  // repaint check belongs behind it, so a pending render requested elsewhere
  // is not re-requested on every idle pass.
  struct TouchRoute {
    freeink::ui::ActionEvent event{};
    freeink::ui::InputSnapshot snap{};
    bool routed = false;
    explicit operator bool() const { return static_cast<bool>(event); }
  };

  // Gated snapshot-build + route: the common loop head. withLongPress forwards
  // the SDK long-press (rows must carry InputLongPress); routeHeld forwards
  // held frames for InputDrag elements (sliders, drag-select fields).
  TouchRoute routeTouch(const MappedInputManager& input, bool withLongPress = false, bool routeHeld = false);

 protected:
  // Called with the snapshot that is about to be dispatched, before app.route()
  // runs its handlers. The hook exists for touch acknowledgment: a handler is
  // free to finish the activity or open another screen, so anything that wants
  // to draw on the element the finger landed on has to do it first. Keeping it
  // here rather than in the caller leaves the uiReady gate and the one-shot
  // snapshot build in a single place (building the snapshot twice would consume
  // the tap twice).
  virtual void onBeforeRoute(const freeink::ui::InputSnapshot&) {}

 public:
  // Gated route of a caller-built snapshot, for flows that need the snapshot
  // before dispatch (e.g. a handler that reads "was this a release" state).
  freeink::ui::ActionEvent route(const freeink::ui::InputSnapshot& snap);

  // --- Swipe to delete (touch boards) ------------------------------------------
  // A left swipe on a list row reveals a trash button at the row's end; tapping it asks the
  // screen to delete that row. A right swipe, Back, Confirm, or a tap anywhere else hides it,
  // and an up/down swipe hides it and still scrolls.
  //
  // Reserved for the trash button, far above any screen's own action ids.
  static constexpr freeink::ui::ActionId ACTION_SWIPE_DELETE = 0xFF00;
  struct SwipeDelete {
    enum class Kind : uint8_t { None, Consumed, Delete };
    Kind kind = Kind::None;
    int row = -1;  // the row to delete, for Kind::Delete
  };
  // Loop task, before routeTouch(). rowAction is the list's row action; canDelete(ctx, row)
  // says whether that row may be revealed. Consumed and Delete both want a repaint.
  SwipeDelete handleSwipeDelete(const MappedInputManager& input, freeink::ui::ActionId rowAction, bool withLongPress,
                                bool (*canDelete)(void*, int), void* ctx);
  // Render task, right before screen.list(props): shows the trash on the revealed row.
  void configureSwipeDelete(const UiScreen& screen, freeink::ui::ListProps& props);
  // Hides the trash. True when one was showing, so the caller knows to repaint.
  bool closeSwipeDelete() { return revealedRow.exchange(-1) >= 0; }
  bool swipeDeleteOpen() const { return revealedRow.load() >= 0; }

  // Close the routing gate outside a render, e.g. when the data the
  // interaction table indexes is released mid-state. Reopens on renderUi().
  void closeRouting() { uiReady = false; }
  bool routingReady() const { return uiReady.load(); }

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;

 private:
  // Opened by the render task after publication and closed on lifecycle/state
  // resets; read by the loop task (route*).
  std::atomic<bool> uiReady{false};
  // Row whose trash button shows, or -1. Written by the loop task, read by the render task.
  std::atomic<int16_t> revealedRow{-1};
  // Filled on the render task for props.reveal; ListProps only points at it.
  freeink::ui::ListRevealAction swipeReveal{};
};
