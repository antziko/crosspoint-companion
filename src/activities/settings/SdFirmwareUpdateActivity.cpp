#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <esp_ota_ops.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  // Build-identity marker — confirms which firmware build owns the SD update flow.
  LOG_INF("FW", "SdFirmwareUpdateActivity build=%s %s recovery=%d", __DATE__, __TIME__, recoveryMode ? 1 : 0);
  state = State::PICKING;
  launchPicker();
}

void SdFirmwareUpdateActivity::launchPicker() {
  // Reuse the standard file browser, restricted to .bin files only.
  startActivityForResultNoThrow<FileBrowserActivity>([this](const ActivityResult& result) { onPickerResult(result); },
                                                     renderer, mappedInput, "/",
                                                     FileBrowserActivity::Mode::PickFirmware);
}

void SdFirmwareUpdateActivity::onPickerResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      // Recovery mode: re-launch the picker so the user cannot escape into a half-initialised UI.
      launchPicker();
      return;
    }
    finish();
    return;
  }

  const auto* path = std::get_if<FilePathResult>(&result.data);
  if (!path) {
    LOG_ERR("FW", "Picker returned no path");
    finish();
    return;
  }
  firmwarePath = path->path;
  LOG_DBG("FW", "Selected: %s", firmwarePath.c_str());

  {
    RenderLock lock(*this);
    state = State::VALIDATING;
  }
  requestUpdateAndWait();

  if (!validateFirmware()) {
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  promptConfirmation();
}

bool SdFirmwareUpdateActivity::validateFirmware() {
  HalFile file;
  if (!Storage.openFileForRead("FW", firmwarePath.c_str(), file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_OPEN_FAILED);
    return false;
  }
  firmwareSize = file.fileSize();
  file.close();

  // Resolve the next-update partition directly via the OTA API. Previously this
  // probed via Update.begin(firmwareSize)/Update.abort() to learn the partition
  // size, which had the side effect of erasing partition state and was wasted
  // work since we only need the size bound for validation here.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FW", "no next-update partition available");
    errorMessage = tr(STR_INVALID_FIRMWARE);
    return false;
  }
  const size_t partitionLimit = dest->size;
  if (firmwareSize > partitionLimit) {
    LOG_ERR("FW", "firmware (%u bytes) exceeds partition (%u bytes)", static_cast<unsigned>(firmwareSize),
            static_cast<unsigned>(partitionLimit));
    errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    return false;
  }

  // Run the same end-to-end integrity check (header / segment table / XOR checksum / SHA256
  // trailer) that the shared firmware-flasher applies right before raw-writing otadata. This
  // catches truncated or corrupted .bin files at confirmation time, before the user ever sees
  // the "Updating…" progress bar.
  const auto vr = firmware_flash::validateImageFile(firmwarePath.c_str(), partitionLimit);
  if (vr != firmware_flash::Result::OK) {
    LOG_ERR("FW", "image validation failed: %s", firmware_flash::resultName(vr));
    if (vr == firmware_flash::Result::TOO_LARGE) {
      errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    } else if (vr == firmware_flash::Result::TOO_SMALL) {
      errorMessage = tr(STR_FIRMWARE_TOO_SMALL);
    } else if (vr == firmware_flash::Result::BAD_CHIP || vr == firmware_flash::Result::WRONG_BOARD) {
      errorMessage = tr(STR_FIRMWARE_WRONG_DEVICE);
    } else {
      errorMessage = tr(STR_INVALID_FIRMWARE);
    }
    return false;
  }
  return true;
}

void SdFirmwareUpdateActivity::promptConfirmation() {
  {
    RenderLock lock(*this);
    state = State::CONFIRMING;
  }
  // Show "Update firmware?" with the file path as the body line.
  std::string heading = tr(STR_FIRMWARE_UPDATE_PROMPT);
  // Use the basename only to keep the body short.
  std::string body = firmwarePath;
  const auto pos = body.find_last_of('/');
  if (pos != std::string::npos) body = body.substr(pos + 1);

  startActivityForResultNoThrow<ConfirmationActivity>(
      [this](const ActivityResult& result) { onConfirmationResult(result); }, renderer, mappedInput, heading, body);
}

void SdFirmwareUpdateActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      // Go back to the picker rather than exiting recovery.
      launchPicker();
      return;
    }
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::UPDATING;
    writtenBytes = 0;
    lastRenderedPercent = 101;
    progressFrames = 0;
    updateStartMs = millis();
  }
  requestUpdateAndWait();
  performUpdate();
}

void SdFirmwareUpdateActivity::performUpdate() {
  LOG_INF("FW", "SD update: %s (%u bytes)", firmwarePath.c_str(), static_cast<unsigned>(firmwareSize));

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<SdFirmwareUpdateActivity*>(ctx);
    self->writtenBytes = written;
    self->firmwareSize = total;
    // immediate=true: wake the render task directly. We're in a tight sync
    // loop so the main loop won't drain the requestedUpdate flag for us.
    self->requestUpdate(true);
  };

  // Re-validate at flash time (TOCTOU): SD is removable, so don't trust the
  // pre-confirmation pass. The alreadyValidated parameter on the API stays
  // for callers (e.g. an OTA staging path) where the same byte stream was
  // just hashed and there's no removable-media gap.
  const auto result = firmware_flash::flashFromSdPath(firmwarePath.c_str(), progressCb, this);
  if (result != firmware_flash::Result::OK) {
    LOG_ERR("FW", "flash failed: %s", firmware_flash::resultName(result));
    // BAD_CHIP / WRONG_BOARD here is the TOCTOU re-validation catching a
    // wrong-device image the pre-confirmation pass missed (e.g. the SD card
    // was swapped).
    errorMessage = result == firmware_flash::Result::BAD_CHIP || result == firmware_flash::Result::WRONG_BOARD
                       ? tr(STR_FIRMWARE_WRONG_DEVICE)
                       : tr(STR_FIRMWARE_WRITE_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("FW", "SD firmware update complete, restarting");
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  requestUpdateAndWait();
  delay(1500);
  ESP.restart();
}

void SdFirmwareUpdateActivity::logUpdateDiagnostics(unsigned long cleanMs) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Deduct the deep clean: it runs before this call, but it is a white/black panel
  // sequence, not the UPDATING layout. dwellMs has to mean "how long the progress
  // screen was held", because dwell is the quantity the whole burn-in theory turns on.
  const unsigned long elapsedMs = updateStartMs > 0 ? millis() - updateStartMs : 0;
  const unsigned long dwellMs = elapsedMs > cleanMs ? elapsedMs - cleanMs : elapsedMs;
  // headerY is the exact panel row the themed header rule would have occupied. If a
  // burned line is reported, it should measure at this y — that is the check that
  // confirms (or kills) the header-rule explanation.
  const int headerY = metrics.topPadding + metrics.headerHeight - metrics.headerUnderlineSize;
  SdDebugLog::setEnabled(true);
  // The absence of this line is itself diagnostic: it means the firmware that performed
  // the write predates these changes, so neither the header-rule removal nor the deep
  // clean ran — the burn seen afterwards was produced by the old code, not by this one.
  SdDebugLog::log("FWU",
                  "sd-update ok size=%u frames=%u dwellMs=%lu cleanMs=%lu theme=%d underline=%d headerY=%d recovery=%d",
                  static_cast<unsigned>(firmwareSize), progressFrames, dwellMs, cleanMs,
                  static_cast<int>(SETTINGS.uiTheme), metrics.headerUnderlineSize, headerY, recoveryMode ? 1 : 0);
}

void SdFirmwareUpdateActivity::loop() {
  if (state == State::FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      if (recoveryMode) {
        // Go back to picker so user can try a different .bin
        state = State::PICKING;
        launchPicker();
        return;
      }
      finish();
    }
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  unsigned int pct = 0;
  if (state == State::UPDATING) {
    // Throttle redraws to once per PROGRESS_STEP_PERCENT, not once per percent.
    // Checked before any drawing so a throttled callback does no work at all.
    //
    // This is a throughput fix, not the ghosting fix: on X3 the SD card shares the
    // display SPI bus, so every repaint here stalls the flash writes it sits between.
    // It does NOT reduce panel residue — displayBuffer() below defaults to FAST_REFRESH,
    // and a differential waveform gives no drive at all to a pixel whose value does not
    // change (Uc8253X3Driver.cpp:191-206), so pixels identical across every frame are
    // unaffected by how many frames there are. See the header note below for what
    // actually burns.
    pct = firmwareSize > 0 ? static_cast<unsigned int>((writtenBytes * 100) / firmwareSize) : 0;
    const unsigned int step = pct - (pct % PROGRESS_STEP_PERCENT);
    if (step == lastRenderedPercent) {
      return;
    }
    lastRenderedPercent = step;
    progressFrames++;
  }

  renderer.clearScreen();

  const char* headerText = recoveryMode ? tr(STR_RECOVERY_MODE) : tr(STR_SD_FIRMWARE_UPDATE);
  if (state == State::UPDATING) {
    // Plain title instead of the themed header, deliberately: GUI.drawHeader draws a
    // solid black full-width rule under a titled band (BaseTheme.cpp:484-487 — 3px on
    // Lyra/Lyra-3/Vega, 0 on Classic/RoundedRaff), landing at y = topPadding +
    // headerHeight - 3. A firmware write holds one layout for tens of seconds to
    // minutes, so that rule would sit at solid DC black on the same three rows for the
    // whole flash. That dwell — not the frame count — is what sets e-ink image sticking,
    // and it shows up later as a faint line across the sleep wallpaper, the only
    // full-screen content with nothing drawn at that y. Text glyphs are thin and sparse,
    // so the title itself is not a comparable risk.
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + (metrics.headerHeight - lineHeight) / 2, headerText,
                              true, EpdFontFamily::BOLD);
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerText);
  }

  const auto top = (pageHeight - lineHeight) / 2;

  if (state == State::VALIDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_FIRMWARE));
  } else if (state == State::UPDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);

    int y = top + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(pct), 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the do-not-power-off line below stays at the same Y as before.
    y += lineHeight + metrics.verticalSpacing;
    // Wrap the warning over up to 2 lines instead of clipping at the screen edge.
    const auto warnLines =
        renderer.wrappedText(UI_10_FONT_ID, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF), pageWidth - 40, 2);
    for (const auto& line : warnLines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += lineHeight;
    }
  } else if (state == State::SUCCESS) {
    // Deep clean before the reboot, while we still control the panel. This is the moment
    // right after the longest unbroken static-black dwell the UI ever produces, and it is
    // the last chance to clear it: nothing after ESP.restart() can, so the residue would
    // otherwise be inherited by every screen the new firmware draws — including the next
    // sleep image, where it is finally visible.
    //
    // A single FULL_REFRESH (what this used to be) is one inversion cycle and does not
    // release sticking set over minutes; deepCleanPanel runs several. It leaves the panel
    // and framebuffer white, so the success text below is drawn onto a clean buffer and
    // pushed by the terminal FULL at the end of this function.
    //
    // The user sees a few black/white flashes for ~15s before "Update complete". That is
    // acceptable here — they are already waiting on a flash and told not to power off —
    // and it is the only automatic clear in the firmware.
    logUpdateDiagnostics(renderer.deepCleanPanel());
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    // Wrap the restart hint ("...hold the power for a few seconds...") over up to 3
    // lines instead of a single centered line that runs off both edges (X3 narrower).
    const int hintY = top + lineHeight + metrics.verticalSpacing;
    const Rect hintBounds{metrics.contentSidePadding, hintY, pageWidth - metrics.contentSidePadding * 2,
                          pageHeight - hintY};
    UITheme::drawCenteredWrappedText(renderer, hintBounds, UI_10_FONT_ID, tr(STR_RESTARTING_HINT), 3, true,
                                     EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      // Wrap so a long detail can't run off both screen edges (consistency with
      // the other error screens; X3 is narrower than X4).
      const auto errLines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), pageWidth - 40, 3);
      int errY = top + lineHeight + metrics.verticalSpacing;
      for (const auto& line : errLines) {
        renderer.drawCenteredText(UI_10_FONT_ID, errY, line.c_str());
        errY += lineHeight;
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // PICKING / CONFIRMING: a sub-activity is on top, nothing to draw.
    if (recoveryMode) {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_RECOVERY_MODE_HINT));
    }
  }

  // Terminal states get a full flash: SUCCESS is the last frame before ESP.restart() (and
  // follows the deep clean above, so it paints from a white panel), and FAILED is read
  // then navigated away from. Both are rare and one-shot, so the ~2s costs nothing.
  // UPDATING stays on the default FAST — a full flash per step would add ~20s to a flash
  // whose repaints already contend with the SD card for the shared SPI bus.
  const bool terminal = state == State::SUCCESS || state == State::FAILED;
  renderer.displayBuffer(terminal ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}
