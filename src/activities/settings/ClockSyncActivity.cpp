#include "ClockSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ClockSyncPolicy.h"

void ClockSyncActivity::onEnter() {
  Activity::onEnter();
  state = PICKING_WIFI;
  syncedTime[0] = '\0';
  // Hand back the 32KB inflate window before the radio takes its ~53KB. The picker releases
  // it too, but pushActivity() is deferred — its onEnter runs a tick later, so that release
  // lands after the picker's own activity object has already been allocated here, against
  // this heap. Without this that allocation meets ~2KB free and the screen does nothing
  // until a reboot. Idempotent, so the picker's call is a no-op; nothing here inflates, and
  // onExit() reboots, which re-reserves the window.
  InflateReader::releaseWindow();

  // Bring up the radio and let the user pick a network first (saved networks
  // connect with one tap). Once connected we run the NTP sync.
  WiFi.mode(WIFI_STA);
  startActivityForResultNoThrow<WifiSelectionActivity>(
      [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); }, renderer, mappedInput);
}

void ClockSyncActivity::onExit() {
  Activity::onExit();

  // Release the radio cleanly. The WiFi stack can leave the SDK in a state that
  // upsets later SD/SPI use, so silently restart once we're done and return to
  // the Reader settings list (matches FontDownloadActivity).
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToSettings(/*Reader=*/1);
  }
}

void ClockSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  state = SYNCING;
  // Render the "Syncing..." screen before the blocking NTP call.
  requestUpdateAndWait();
  runSync();
}

void ClockSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("CLK", "Manual sync requested but WiFi is not connected");
    state = NO_WIFI;
    requestUpdate();
    return;
  }

  // 20 s, not the 5 s default. Two blocking WiFi.hostByName() calls run inside syncFromNTP()
  // before its poll even starts, and SNTP's first packet can arrive after that -- at 5 s the
  // screen declared FAILED while the sync was still in flight, then usually completed a moment
  // later (syncFromNTP leaves _ntpConfigured set precisely so a late packet is still adopted).
  // The user is watching a "Syncing..." screen here, so waiting is the honest behaviour; the
  // background sync task already uses 20 s (main.cpp maybeStartBackgroundNtpSync).
  static constexpr uint32_t kManualSyncTimeoutMs = 20000;
  const bool ok = halClock.syncFromNTP(kManualSyncTimeoutMs);
  if (!ok) {
    state = FAILED;
    requestUpdate();
    return;
  }

  // Stamp the sync so the staleness rule restarts from here (and the auto-sync hook stops
  // firing on every WiFi connect).
  noteClockSynced();

  // Read the freshly synced time back for the user-facing confirmation.
  char buf[9];
  if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    snprintf(syncedTime, sizeof(syncedTime), "%s", buf);
  }
  state = SUCCESS;
  requestUpdate();
}

void ClockSyncActivity::loop() {
  // WiFi selection and the (blocking) sync run from onWifiSelectionComplete.
  // Here we only wait for the user to dismiss the result screen.
  if (state == PICKING_WIFI || state == SYNCING) return;

  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void ClockSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_SYNC));

  const int midY = pageHeight / 2;

  switch (state) {
    case SYNCING:
      renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_CLOCK_SYNCING));
      break;
    case SUCCESS: {
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_OK), true, EpdFontFamily::BOLD);
      if (syncedTime[0] != '\0') {
        // Sized for the label in any language: STR_CURRENT_TIME is 26 bytes in
        // Russian (UTF-8 Cyrillic is 2 bytes per letter) versus 13 in English,
        // plus a separator and up to "08:56 PM".
        char line[64];
        snprintf(line, sizeof(line), "%s %s", tr(STR_CURRENT_TIME), syncedTime);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      }
      break;
    }
    case NO_WIFI:
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_NO_WIFI), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CLOCK_SYNC_NO_WIFI_HINT));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  if (state != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
