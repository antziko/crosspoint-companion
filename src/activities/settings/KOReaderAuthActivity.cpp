#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <InflateReader.h>
#include <SdDebugLog.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = mode == Mode::SIGN_UP ? tr(STR_CREATING_ACCOUNT) : tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  // Reclaim the 32KB inflate window (reserved at boot in main.cpp) for the TLS
  // handshake. Settings-context auth never inflates, and onExit() always reboots
  // (silentRestartToSettings) — which re-reserves the window on a fresh heap — so
  // it is never re-allocated here. Without this, X3 has only ~53KB free during
  // the handshake, below MIN_HEAP_FOR_TLS, and HTTPS auth fails with LOW_MEMORY.
  // Mirrors KOReaderSyncActivity's reader-context release.
  InflateReader::releaseWindow();

  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = mode == Mode::SIGN_UP ? KOReaderSyncClient::createUser() : KOReaderSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS);
      // targetServerIndex was set active in onEnter(); leave it as the sync server.
    } else {
      state = FAILED;
      errorMessage =
          result == KOReaderSyncClient::USER_EXISTS ? tr(STR_USERNAME_TAKEN) : KOReaderSyncClient::errorString(result);
      // Restore the previous active server so a failed auth doesn't change the sync server.
      if (targetServerIndex >= 0 && previousActiveIndex >= 0) {
        KOREADER_STORE.setActiveIndex(previousActiveIndex);
        KOREADER_STORE.saveToFile();
      }
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  Activity::onEnter();

  // X3 HTTPS troubleshooting: enable the SD trace for the lifetime of this
  // activity (covers KOReaderSyncClient::authenticate). See SdDebugLog.h /
  // SUMMARY.md Part B Appendix.
  SdDebugLog::setEnabled(true);

  // If a specific server should be tested, make it active now so the sync client reads its creds.
  if (targetServerIndex >= 0) {
    previousActiveIndex = KOREADER_STORE.getActiveIndex();
    KOREADER_STORE.setActiveIndex(targetServerIndex);
    KOREADER_STORE.saveToFile();
  }

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderAuthActivity::onExit() {
  Activity::onExit();

  SdDebugLog::setEnabled(false);

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToSettings(/*System=*/3);
  }
}

void KOReaderAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 mode == Mode::SIGN_UP ? tr(STR_SIGN_UP) : tr(STR_KOREADER_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top,
                              mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, mode == Mode::SIGN_UP ? tr(STR_SIGNUP_FAILED) : tr(STR_AUTH_FAILED),
                              true, EpdFontFamily::BOLD);
    // Wrap the detail over up to 3 lines instead of a single centered line that
    // runs off both screen edges (the LOW_MEMORY string is long; X3 is narrower
    // than X4). Mirrors the wrappedText pattern in OpdsBookBrowserActivity.
    const auto errLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), pageWidth - 40, 3);
    int errY = top + height + 10;
    for (const auto& line : errLines) {
      renderer.drawCenteredText(UI_10_FONT_ID, errY, line.c_str());
      errY += height;
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void KOReaderAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}
