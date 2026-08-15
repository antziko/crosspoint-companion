#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * SD-card based firmware update activity.
 *
 * Flow:
 *  1) onEnter -> push FileBrowserActivity in PickFirmware mode (only .bin files visible).
 *  2) On result: validate the .bin (header magic, size fits OTA partition).
 *  3) Push ConfirmationActivity ("Update firmware?").
 *  4) On confirm: stream the file into the OTA partition via the Arduino Update API,
 *     drawing a progress bar; on success ESP.restart().
 *
 * Used both from Settings -> System -> "SD Card Firmware Update", and as the only
 * activity launched in boot recovery mode (left side button + power on X3).
 */
class SdFirmwareUpdateActivity : public Activity {
 public:
  enum class State {
    PICKING,
    VALIDATING,
    CONFIRMING,
    UPDATING,
    SUCCESS,
    FAILED,
  };

  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool recoveryMode = false)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), recoveryMode(recoveryMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::UPDATING || state == State::VALIDATING; }
  bool skipLoopDelay() override { return state == State::UPDATING; }

 private:
  State state = State::PICKING;
  bool recoveryMode = false;

  std::string firmwarePath;
  size_t firmwareSize = 0;
  size_t writtenBytes = 0;
  // Progress redraw granularity. Each redraw is a full-screen differential refresh, and a
  // long run of those is what burns a residual band into the panel (see render()). 10%
  // keeps the bar visibly moving on a flash that takes tens of seconds while cutting the
  // frame count from ~100 to 10. Must divide 100 so the last step lands on a drawn frame.
  static constexpr unsigned int PROGRESS_STEP_PERCENT = 10;
  // Sentinel: outside 0..100 so the first progress callback always draws.
  unsigned int lastRenderedPercent = 101;
  // Troubleshooting counters for the panel-burn investigation: how many progress
  // frames were actually painted, and how long the UPDATING layout was held on
  // screen. Dwell (not frame count) is what sets image sticking, so both are
  // logged on the terminal transition.
  unsigned int progressFrames = 0;
  unsigned long updateStartMs = 0;
  std::string errorMessage;

  void launchPicker();
  void onPickerResult(const ActivityResult& result);
  bool validateFirmware();
  void promptConfirmation();
  void onConfirmationResult(const ActivityResult& result);
  void performUpdate();
  // Panel-burn troubleshooting: records the UPDATING dwell, painted frame count and the
  // active theme's header-rule thickness to the SD debug log, immediately before the
  // reboot that would otherwise lose them.
  void logUpdateDiagnostics(unsigned long cleanMs) const;
};
