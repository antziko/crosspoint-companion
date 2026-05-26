#include "ClockSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ClockSyncActivity::onEnter() {
  Activity::onEnter();
  displayedTime[0] = '\0';
  wifiLaunchAttempted = false;
  // On RTC devices, show the current time and ask the user to confirm an NTP refresh rather
  // than force-resyncing. RAM mode has no preserved time, so skip the prompt and run sync.
  if (halClock.hasRtc() && halClock.hasTime()) {
    state = CONFIRM_RTC;
    char buf[9];
    if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      snprintf(displayedTime, sizeof(displayedTime), "%s", buf);
    }
  } else {
    state = SYNCING;
  }
  requestUpdate();
}

void ClockSyncActivity::onExit() { Activity::onExit(); }

void ClockSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    // WiFi is intentionally off on these devices outside of explicit use. Bring up the
    // selection screen so the user can pick a network and connect; we'll re-enter runSync
    // once it returns. Only attempted once per activity to avoid a relaunch loop if the
    // user backs out without connecting.
    if (wifiLaunchAttempted) {
      LOG_INF("CLK", "Manual sync requested but user cancelled WiFi selection");
      state = NO_WIFI;
      requestUpdate();
      return;
    }
    wifiLaunchAttempted = true;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               // User declined to connect. Show the NO_WIFI hint and wait for Back.
                               state = NO_WIFI;
                               requestUpdate();
                             }
                             // On success: state stays SYNCING so loop() re-invokes runSync() with WiFi up.
                           });
    return;
  }

  const bool ok = halClock.syncFromNTP();
  if (!ok) {
    state = FAILED;
    requestUpdate();
    return;
  }

  // Persistent debounce flag is RTC-only — it suppresses re-NTP across reboots.
  // RAM-mode debouncing is handled by HalClock::syncedThisSession() and resets each boot.
  if (halClock.hasRtc()) {
    SETTINGS.clockHasBeenSynced = 1;
    SETTINGS.saveToFile();
  }

  // Read the freshly synced time back for the user-facing confirmation.
  char buf[9];
  if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    snprintf(displayedTime, sizeof(displayedTime), "%s", buf);
  }
  state = SUCCESS;
  requestUpdate();
}

void ClockSyncActivity::loop() {
  if (state == CONFIRM_RTC) {
    // Confirm = refresh from NTP; Back = dismiss without syncing. Use wasReleased so the
    // event can't leak back into the parent activity and re-trigger this screen.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = SYNCING;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }
  if (state == SYNCING) {
    // First-tick: render the "Syncing..." screen, then perform the (blocking) sync.
    // requestUpdateAndWait below forces the render before we block on WiFi.
    requestUpdateAndWait();
    runSync();
    return;
  }

  // Terminal states (SUCCESS/FAILED/NO_WIFI). Wait for release so the event can't leak.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
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
    case CONFIRM_RTC: {
      // Show the current RTC time then ask the user whether to refresh from NTP.
      if (displayedTime[0] != '\0') {
        char line[32];
        snprintf(line, sizeof(line), "%s %s", tr(STR_CURRENT_TIME), displayedTime);
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, line, true, EpdFontFamily::BOLD);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CLOCK_REFRESH_FROM_NTP));
      break;
    }
    case SYNCING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_CLOCK_SYNCING));
      break;
    case SUCCESS: {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_OK), true, EpdFontFamily::BOLD);
      if (displayedTime[0] != '\0') {
        char line[32];
        snprintf(line, sizeof(line), "%s %s", tr(STR_CURRENT_TIME), displayedTime);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      }
      break;
    }
    case NO_WIFI:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_NO_WIFI), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CLOCK_SYNC_NO_WIFI_HINT));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  if (state == CONFIRM_RTC) {
    const auto labels = mappedInput.mapLabels(tr(STR_NO), tr(STR_YES), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
