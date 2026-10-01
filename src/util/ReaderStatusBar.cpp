#include "ReaderStatusBar.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <cstdio>

#include "CrossPointState.h"

namespace {
constexpr uint8_t FILE_VERSION = 1;
// "<cache dir>/statusbar.bin" of the open book; empty while no reader has one open.
char filePath[128] = "";
}  // namespace

namespace ReaderStatusBar {

void load(const std::string& bookCachePath) {
  APP_STATE.statusBarHidden = false;
  const int n = snprintf(filePath, sizeof(filePath), "%s/statusbar.bin", bookCachePath.c_str());
  if (n <= 0 || n >= static_cast<int>(sizeof(filePath))) {
    LOG_ERR("RSB", "Cache path too long: %s", bookCachePath.c_str());
    filePath[0] = '\0';
    return;
  }
  HalFile f;
  if (!Storage.openFileForRead("RSB", filePath, f)) return;  // never toggled: bar shown
  uint8_t data[2];
  if (f.read(data, 2) == 2 && data[0] == FILE_VERSION) {
    APP_STATE.statusBarHidden = data[1] != 0;
  }
}

void unload() {
  APP_STATE.statusBarHidden = false;
  filePath[0] = '\0';
}

void toggle() {
  APP_STATE.statusBarHidden = !APP_STATE.statusBarHidden;
  if (filePath[0] == '\0') return;
  HalFile f;
  if (!Storage.openFileForWrite("RSB", filePath, f)) {
    LOG_ERR("RSB", "Failed to save status bar state");
    return;
  }
  const uint8_t data[2] = {FILE_VERSION, static_cast<uint8_t>(APP_STATE.statusBarHidden ? 1 : 0)};
  f.write(data, 2);
}

}  // namespace ReaderStatusBar
