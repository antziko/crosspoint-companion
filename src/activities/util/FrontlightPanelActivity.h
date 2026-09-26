#pragma once

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored control center opened by a top-edge down-swipe, a status-bar tap,
// or a button bound to "Control Center" (iOS Control Center style): a grabber,
// the frontlight brightness/warmth sliders (on boards with a light), and a grid
// of quick-setting tiles — night mode, ghost-cleanup refresh, reading
// orientation, and reader touch controls on/off. The
// frontlight controls are always there: they are what the panel is for. Pure
// 1-bit: no dithered fills, selection reads as a filled tile. The grabber sits
// along the panel's bottom edge, the edge the sheet is dragged from.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which
  // legitimately diverges from the saved SETTINGS.frontlightOn preference —
  // e.g. after a wake with frontlightRestoreOnWake off, the light stays off
  // live while the saved "was on" preference is deliberately kept (see
  // main.cpp's restoreLightOn). brightness/warmth have no such divergence
  // (always restored unconditionally on boot), so only lightOn needs a
  // touched-by-the-user flag: onExit() must not persist a mirror that never
  // reflected user intent in the first place.
  bool lightOnChanged = false;
  bool draggingSlider = false;
  // Night-mode polarity as the sheet opened. close() compares it: a flip rewrites every pixel of
  // the screen underneath, which is the one exit from this sheet that has to scrub.
  uint8_t enteredInverted = 0;
  // The touch tile toggles SETTINGS.touchReaderControls between off and this
  // remembered mode, so a Swipe or Inverted Tap user gets their mode back
  // rather than the Tap default. Seeded from the setting in onEnter().
  uint8_t touchModeRestore = CrossPointSettings::TOUCH_READER_ON;
  // Reading orientation the tile shows and cycles from. Held here because the tile does
  // not always write SETTINGS.orientation: over an open book the rotation is the book's
  // own, so the global default it would otherwise read back stays on the old value.
  // Seeded from APP_STATE.activeOrientation in onEnter().
  uint8_t orientationShown = 0;
  int panelBottom = 0;

  // Quick-setting tiles, in grid order (2 columns): night mode, refresh,
  // orientation, touch. Fixed set — shown on touch boards, absent elsewhere.
  static constexpr int kTileCount = 4;

  // fui::SliderRowProps and fui::TileGridProps embed a 324-byte fui::StyleSet,
  // so the props the render path fills in live here instead of on the stack
  // (AGENTS.md: locals stay under 256 bytes). The components take them by
  // const reference and draw immediately, so one instance per call site is
  // enough — every field either is reassigned on each use or keeps its
  // constructed default.
  freeink::ui::SliderRowProps rowProps;
  freeink::ui::TileGridProps gridProps;
  freeink::ui::TileGridItem gridItems[kTileCount];

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTileEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // One slider row: a caption line (name + live percentage) above
  // [-] [draggable 1-bit capsule] [+], plus a lamp on/off button after the +
  // when showToggle is set (the brightness row).
  void addSliderRow(UiScreen& screen, const char* label, uint8_t value, freeink::ui::ActionId sliderAction,
                    freeink::ui::ActionId stepAction, bool showToggle);
  int computePanelBottom() const;
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void runTile(int idx);
  // Copy the panel's live brightness/warmth/lightOn into SETTINGS and save if
  // anything actually changed. onExit() runs it on every way out.
  void persistLightSettings();
  void close();

  // One-shot: a tile that rewrote the whole frame (night mode) re-drives it
  // with the ghost-cleanup waveform on the next render. The "refresh" tile does
  // not use this — it closes the panel and promotes the repaint underneath
  // instead (GfxRenderer::promoteNextRefresh).
  bool cleanRefreshPending = false;

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;
  // The sheet is a glance-and-dismiss overlay: night mode's entry scrub skips opening it and
  // closing it. close() arms its own scrub for the two tiles that change the frame underneath
  // wholesale (polarity, orientation).
  bool isTransientScreen() const override { return true; }
};
