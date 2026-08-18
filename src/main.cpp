#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <SPI.h>
#include <SdDebugLog.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SettingsPersistence.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/SleepImageReviewActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "network/NtpBgState.h"
#include "util/ButtonNavigator.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
DictionaryRegistry dictionaryRegistry;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());

// X4 Pro power-button timing. The board has no dedicated light key, so a double
// click of POWER toggles the frontlight; a single click still runs the
// configured short-press action once the double-click window has passed.
namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;
constexpr unsigned long X4PRO_RECOVERY_SETTLE_MS = 20;
constexpr unsigned long DEFAULT_RECOVERY_SETTLE_MS = 500;
}  // namespace
static unsigned long lastX4ProPowerClickAt = 0;

static unsigned long allowSleepAt = 0;
// A wake hold must never become an in-app power-button action. Boot may finish while the
// button is still held, so swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ipaFont(&ipa_16_regular);
EpdFontFamily ipaFontFamily(&ipaFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootSettingsCategory;  // category index for SETTINGS target
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 2;  // settings list (category in silentRebootSettingsCategory)

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag.
                   // Quick Resume repaints the saved frame; a custom sleep image paints
                   // nothing and leaves the retained image up.
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// Power the modem all the way down before a soft reset. Callers only WiFi.disconnect()
// (radio left on); leaving it alive across ESP.restart() hangs early boot on X4 — the
// reader's heavy SD/section load after the reboot tips it over, so the device sticks on
// the "Loading" frame. Deep-sleep teardown does this same WIFI_OFF and resumes cleanly.
static void wifiPowerDownForReboot() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  wifiPowerDownForReboot();
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  halClock.persistTimeAcrossReboot();  // X4: carry NTP-synced time across the soft reset
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  wifiPowerDownForReboot();
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  halClock.persistTimeAcrossReboot();  // X4: carry NTP-synced time across the soft reset
  delay(50);
  ESP.restart();
}

void silentRestartToSettings(int category) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  wifiPowerDownForReboot();
  silentRebootTarget = SILENT_REBOOT_TARGET_SETTINGS;
  silentRebootSettingsCategory = static_cast<uint32_t>(category);
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=settings,cat=%d)", category);
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  halClock.persistTimeAcrossReboot();  // X4: carry NTP-synced time across the soft reset
  delay(50);
  ESP.restart();
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep it
  // there until the first real reader/home paint replaces it instead of overwriting
  // it with the splash.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame left by an earlier sleep must not replace the
    // selected sleep screen during wake by painting the old reader page over it.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.insertFont(IPA_FONT_ID, ipaFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

// X4 has no RTC chip, so the system clock is lost on every full boot / deep-sleep
// wake. When any clock-dependent feature is enabled — home top-bar clock/date,
// reader status-bar clock/date — and a WiFi network is saved, silently reconnect
// and sync NTP in a background task so the clock fills in shortly after boot, on
// both quick-resume and splash boots. A valid clock is also what lets reading
// sessions get dated (BookReadingStats), so enabling the status-bar clock/date is
// enough to make X4 stats land on the timeline/heatmap instead of "Undated". No-op
// on X3 (hardware RTC), when time is already valid, when every clock feature is off,
// or when no WiFi network is saved (so non-clock users never power the radio).
static void maybeStartBackgroundNtpSync() {
  if (halClock.hasHardwareRtc() || halClock.isSystemTimeValid()) return;
  if (!SETTINGS.homeTopBarClock && !SETTINGS.homeTopBarDate && !SETTINGS.statusBarClock && !SETTINGS.statusBarDate)
    return;
  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) return;

  struct NtpBgCtx {
    char ssid[33];
    char pass[65];
  };
  static NtpBgCtx ntpBgCtx;
  strncpy(ntpBgCtx.ssid, lastSsid.c_str(), sizeof(ntpBgCtx.ssid) - 1);
  ntpBgCtx.ssid[sizeof(ntpBgCtx.ssid) - 1] = '\0';
  ntpBgCtx.pass[0] = '\0';
  // Snapshot by value: findCredential now returns an optional copy taken under
  // the store's mutex, so nothing here aliases the live credential vector.
  if (const auto cred = WIFI_STORE.findCredential(lastSsid)) {
    strncpy(ntpBgCtx.pass, cred->password.c_str(), sizeof(ntpBgCtx.pass) - 1);
    ntpBgCtx.pass[sizeof(ntpBgCtx.pass) - 1] = '\0';
  }
  // Published for the whole WiFi lifetime (begin .. WIFI_OFF) so the reader-open
  // path can detect the heap-fragmenting window and wait/cancel before loading a
  // book. Set before xTaskCreate so there is no gap where WiFi is coming up but
  // `active` is still false. See src/network/NtpBgState.h.
  NtpBg::cancel = false;
  NtpBg::active = true;
  xTaskCreate(
      [](void* arg) {
        const auto* ctx = static_cast<NtpBgCtx*>(arg);
        WiFi.mode(WIFI_STA);
        if (ctx->pass[0]) {
          WiFi.begin(ctx->ssid, ctx->pass);
        } else {
          WiFi.begin(ctx->ssid);
        }
        for (int i = 0; i < 80 && WiFi.status() != WL_CONNECTED && !NtpBg::cancel; ++i) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (WiFi.status() == WL_CONNECTED) {
          // Heap profiling: radio-up vs radio-off pair below measures the WiFi
          // stack's true footprint outside any activity lifecycle (boot-time sync).
          SdDebugLog::log("MEM", "ntp wifi-up free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
          // This task owns the connection, so wait out a slow SNTP packet
          // (Problem A) before tearing WiFi down — longer than the 5s UI default.
          // NtpBg::cancel cuts the wait short when the user opens a book.
          halClock.syncFromNTP(20000, &NtpBg::cancel);
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          SdDebugLog::log("MEM", "ntp wifi-off free=%u largest=%u", (unsigned)ESP.getFreeHeap(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        } else {
          // Never connected (or cancelled mid-connect): still drop the radio so the
          // STA stack's heap is released before we clear `active`.
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
        }
        NtpBg::active = false;  // heap recovered — reader gate may proceed
        vTaskDelete(nullptr);
      },
      "ntp_bg", 4096, &ntpBgCtx, 1, nullptr);
}

void setup() {
  // Assert the board power-latch rails first thing (before the serial init
  // delay) so the device stays on after the user releases the power button
  // (#2481, new-SDK power model). Idempotent with powerManager.begin() below,
  // which configures the peripheral rails; this only holds the main latch.
  BoardConfig::holdPowerRails();
#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  // checkPanic() clears the watchdog capture marker after a successful SD dump, so
  // latch the boot classification here for the activity route further down.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Reserve the shared 32KB DEFLATE inflate window now, while the heap is pristine,
  // so it is guaranteed contiguous (the §59 "out of bounds" guarantee). Held on the
  // heap rather than BSS so it can be released for a TLS handshake (KOReader sync),
  // with the post-sync reboot re-running this on a fresh heap. Non-fatal on failure:
  // inflate falls back to per-call malloc (pre-reservation behaviour).
  if (!InflateReader::ensureWindow()) {
    LOG_ERR("MAIN", "Inflate window reservation failed; inflate will fall back to malloc");
  }

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_SETTINGS) ? silentRebootTarget : 0;
  // Clamp category to valid range (0-3); RTC_NOINIT can hold garbage on cold boot.
  static constexpr uint32_t SETTINGS_CATEGORY_COUNT = 4;
  const uint32_t snapshotSettingsCategory =
      (isSilentReboot && silentRebootSettingsCategory < SETTINGS_CATEGORY_COUNT) ? silentRebootSettingsCategory : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootSettingsCategory = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  // First of two USB samples (second below, before display bring-up): the SOF
  // verdict needs two samples a frame apart, and it must be settled before the
  // first refresh — the boot paint's light-sleep slices would otherwise kill a
  // live CDC link whenever the charge-based check reads false (full battery,
  // data-only cable). See HalGPIO::pollUsbState().
  gpio.pollUsbState();

  // Light-sleep through the render task's e-ink BUSY wait (0.3-2 s of pure pin
  // polling) in short slices, waking exactly on the BUSY pin's completion level
  // (falls back to plain polling when WiFi/USB blocks light sleep)
  display.setBusyWaitSliceHook(
      [](int8_t busyPin, uint8_t busyLevel) { return powerManager.onEinkBusyWaitSlice(busyPin, busyLevel); });

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    gpio.pollUsbState();  // settle the USB verdict before the error paint (see above)
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  // Development-only: confirm the constexpr persistence table still matches SettingsList.h.
  // Runs before the first load so any drift is logged above the values it would affect.
  // Compiles to nothing in gh_release. Free heap here is ~200 KB, so building the full
  // settings list for the comparison is free.
  verifySettingsPersistenceTable();

  SETTINGS.loadFromFile();
  // Apply the SD-logging toggle now that settings are loaded (default off). Governs
  // the boot-done MEM line below and all later SdDebugLog::log() calls.
  SdDebugLog::setMasterEnabled(SETTINGS.sdCardLogging != 0);
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  // Seed runtime active orientation from the global default. Readers override
  // this per-book; non-reader UI keeps it mirroring the global setting.
  APP_STATE.activeOrientation = SETTINGS.orientation;

  // Brightness and warmth are always restored. A normal wake starts with the light off
  // unless Restore Light on Wake is enabled; a silent maintenance reboot preserves the
  // live state so the screen does not unexpectedly go dark mid-session. Inert on boards
  // without a frontlight.
  const bool restoreLightOn = SETTINGS.frontlightOn != 0 && (SETTINGS.frontlightRestoreOnWake != 0 || isSilentReboot);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  // Clamp lookup history cap to a valid step in [MIN, UNLIMITED] (UNLIMITED is the
  // top sentinel = no eviction).
  if (SETTINGS.lookupHistoryCap < CrossPointSettings::HIST_CAP_MIN ||
      SETTINGS.lookupHistoryCap > CrossPointSettings::HIST_CAP_UNLIMITED ||
      SETTINGS.lookupHistoryCap % CrossPointSettings::HIST_CAP_STEP != 0) {
    SETTINGS.lookupHistoryCap = CrossPointSettings::HIST_CAP_DEFAULT;
  }
  // Validate the stored dictionary path still exists on the SD card.
  Dictionary::isValidDictionary();
  // Discover installed dictionaries for the settings UI (device + web). Mirrors sdFontSystem.begin().
  dictionaryRegistry.discover();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      // verify returns false when the button wasn't held long enough — go back to sleep via the
      // one complete sleep routine (battery-latch + serial teardown), not a HAL-local duplicate.
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP)) {
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold a side button together with the power button at boot to skip
  // directly to the SD-card firmware update screen. Useful on devices where USB flashing has
  // been locked down (e.g. recent X3 firmware). X4 Pro uses BTN_DOWN because its BTN_UP is
  // GPIO0, a boot-strap pin that must not be held during reset; its plain digital buttons also
  // debounce in 5 ms rather than needing the legacy Xteink settling window.
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleMs = BoardConfig::isX4Pro() ? X4PRO_RECOVERY_SETTLE_MS : DEFAULT_RECOVERY_SETTLE_MS;
    const unsigned long settleStart = millis();
    while (millis() - settleStart < settleMs) {
      gpio.update();
      delay(10);
    }
    const uint8_t recoveryButton = BoardConfig::isX4Pro() ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
    if (gpio.isPressed(recoveryButton)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)", BoardConfig::isX4Pro() ? "DOWN" : "UP");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag. Otherwise a
  // stale flag (e.g. a battery pull between enterDeepSleep's save and the wake) could
  // suppress the splash on a cold boot, leaving the panel blank until the first paint.
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const BootResume resume = isSilentReboot                             ? BootResume::Silent
                            : isSleepWake && !APP_STATE.showBootScreen ? BootResume::SplashlessWake
                                                                       : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;

  // Second USB sample (first one right after powerManager.begin()): settles the
  // SOF host-link verdict before the first refresh can slice-sleep.
  gpio.pollUsbState();

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save before any
      // painting so a hang in the blocking paint path can't strand us in a
      // splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      }
      // No frame file: every non-Quick-Resume sleep mode deliberately leaves none
      // (enterDeepSleep removes any stale one), and the panel still holds that mode's
      // retained image. Painting nothing here is the point — the first reader/home
      // paint replaces it.
      // X4: time is lost on deep sleep — background-sync NTP if the clock is on.
      maybeStartBackgroundNtpSync();
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      // X4: full boot also loses time — sync in the background so the home clock
      // refreshes on every wake, not just quick-resume wakes.
      maybeStartBackgroundNtpSync();
      break;
  }

  // On-wake wallpaper review: a random /sleep-folder image was shown entering the last
  // sleep, the feature is on, the file still exists, and it is not already kept. Only the
  // normal home/reader branches below honour this (recovery/panic/silent route earlier).
  const bool reviewSleepImage = SETTINGS.reviewSleepImageOnWake && !APP_STATE.lastSleepImagePath.empty() &&
                                !FsHelpers::isKeptSleepImage(APP_STATE.lastSleepImagePath) &&
                                Storage.exists(APP_STATE.lastSleepImagePath.c_str());

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    auto recovery = makeUniqueNoThrow<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true);
    if (recovery) {
      activityManager.replaceActivity(std::move(recovery));
    } else {
      // Recovery mode is the user's way out of a bad flash; falling back to home at least
      // leaves a usable device instead of aborting the boot.
      LOG_ERR("MAIN", "OOM: SdFirmwareUpdateActivity (recovery); going home");
      activityManager.goHome();
    }
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SETTINGS) {
    // Return to the settings category the user was in before the WiFi reboot.
    activityManager.goToSettings(static_cast<int>(snapshotSettingsCategory));
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    // Nothrow: an OOM here would abort during boot, which reads to the user as a bricked
    // device. Skipping the review and going home loses nothing but the sleep image.
    auto review = reviewSleepImage ? makeUniqueNoThrow<SleepImageReviewActivity>(
                                         renderer, mappedInputManager, APP_STATE.lastSleepImagePath,
                                         /*resumeToReader=*/false, std::string())
                                   : nullptr;
    if (review) {
      activityManager.replaceActivity(std::move(review));
    } else {
      if (reviewSleepImage) LOG_ERR("MAIN", "OOM: SleepImageReviewActivity; going home");
      activityManager.goHome();
    }
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    // Review first, then resume the book (the review activity routes onward). Nothrow: on OOM
    // go straight to the book rather than aborting the boot.
    auto review = reviewSleepImage ? makeUniqueNoThrow<SleepImageReviewActivity>(renderer, mappedInputManager,
                                                                                 APP_STATE.lastSleepImagePath,
                                                                                 /*resumeToReader=*/true, path)
                                   : nullptr;
    if (review) {
      activityManager.replaceActivity(std::move(review));
    } else {
      if (reviewSleepImage) LOG_ERR("MAIN", "OOM: SleepImageReviewActivity; opening the book directly");
      activityManager.goToReader(path, allowFastInitialReaderRefresh);
    }
  }

  if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER) {
    // Seamless boot skips the panel clear, so the first reader paint (fast refresh) would
    // ghost the pre-reboot frame — the KOReader sync "Progress found" screen bleeding
    // through whitish under the page. Force one HALF_REFRESH to wipe it (clears the stale
    // frame without FULL's hard black/white flash).
    renderer.forceCleanRefreshNextPaint();
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Don't block here waiting for the power button to come up: with a custom sleep
  // image the panel is already showing useful content, and spinning in setup() just
  // delays the first real paint. loop() consumes the release instead, via
  // wakePowerReleasePending.
  allowSleepAt = millis() + 2000;

  // Heap profiling baseline: free heap once boot is fully done (fonts, SD, first
  // activity entered). Every later MEM line in the trace diffs against this.
  SdDebugLog::setEnabled(true);
  SdDebugLog::log("MEM", "boot-done %s free=%u largest=%u minEver=%u", gpio.deviceIsX3() ? "X3" : "X4",
                  (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)ESP.getMinFreeHeap());
}

// delay() counts ticks, and the tick stops while onEinkBusyWaitSlice() light-sleeps
// the chip (millis() is RTC-corrected on wake; the tick is not). A delay(10) mid-refresh
// would stretch to ~210 ms and starve button sampling. millis() stays honest.
static void delayWallClock(const unsigned long ms) {
  const unsigned long deadline = millis() + ms;
  while (static_cast<long>(millis() - deadline) < 0) {
    vTaskDelay(1);
  }
}

// X4 Pro: a double click of POWER toggles the frontlight. Returns true when the
// release was consumed as the second click, so the caller skips the configured
// short-press action for it.
static bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !gpio.wasReleased(HalGPIO::BTN_POWER)) return false;

  const unsigned long now = millis();
  // A long hold is the sleep gesture, never half of a double click.
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }

  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s by power-button double-click", lightOn ? "on" : "off");
  return true;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, APP_STATE.activeOrientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Flush any OOM events captured by the operator-new trace to SD (no-op unless a
  // TRACE_OOM_ALLOC build recorded one). This is the only safe context to do the
  // SD write — never from inside the allocation path. Throttled; runs on X3+X4.
  static unsigned long lastOomDrain = 0;
  if (millis() - lastOomDrain >= 1000) {
    HalSystem::drainOomTrace();
    lastOomDrain = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      bool handled = true;
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      } else {
        handled = false;
      }
      // Raw print, not LOG_*: debugging_monitor.py keys on this ack to report
      // command success, so it must survive LOG_LEVEL=0 builds. Commands
      // compiled out of this build report unknown.
      logSerial.printf(handled ? "CMDACK:%s\n" : "CMDERR:unknown:%s\n", cmd.c_str());
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can arrive
  // after setup, so consume that one input frame rather than letting it become a page
  // turn, a forced refresh, or any other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_BACK)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        // Confirm first so a mistaken Power+Back doesn't silently save a screenshot.
        // The modal waits for the combo to release, so on return the buttons are up;
        // the post-combo handler below resets the latch on the next iteration.
        if (ScreenshotUtil::confirmScreenshot(renderer, mappedInputManager)) {
          ScreenshotUtil::takeScreenshot(renderer);
        }
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Deferred manual sleep: an activity asked us to sleep on a later iteration (e.g. the
  // reader's "sync before sleep" flow, after the user chose Skip or the sync completed).
  if (APP_STATE.requestManualSleep) {
    APP_STATE.requestManualSleep = false;
    enterDeepSleep(false);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new in-app
  // long press. Otherwise a user who keeps holding after wake would put the device
  // straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  if (powerReleasedSinceWake && millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_BACK)) {
      return;
    }
    // Offer the gesture to the active activity first. The reader may intercept it to show a
    // "sync before sleep" prompt instead of sleeping immediately. Release the still-held power
    // button before handing over so the prompt isn't dismissed by the same press, and re-arm
    // allowSleepAt so the release doesn't immediately re-trigger this branch.
    waitForPowerRelease();
    allowSleepAt = millis() + 2000;
    if (activityManager.onManualSleepRequested()) {
      return;  // activity took over the gesture; it will request sleep later if appropriate
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // X4 Pro frontlight double-click. Runs before the short-press actions below so
  // the second click toggles the light instead of also firing them.
  if (handleX4ProFrontlightDoubleClick()) {
    lastActivityTime = millis();
    return;
  }
#if FREEINK_CAP_TOUCH
  // A single X4 Pro power click becomes Confirm only once the double-click
  // window has closed, so the first click of a pair is never also a Confirm.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && BoardConfig::isX4Pro() &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    mappedInputManager.setPowerConfirmClickFrame(true);
  } else {
    mappedInputManager.setPowerConfirmClickFrame(false);
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    // Whole-page ghost clear, then re-render (requestUpdate re-runs the active
    // activity's render()). The clear is driven from a blanked framebuffer so it
    // pushes the whole panel — not just changed pixels — clearing ghosting.
    //
    // Mode is user-selectable via SETTINGS.refreshScreenMode (Settings > Display
    // > Refresh Screen Mode), default FAST. Note the panel tradeoff:
    //   - FAST: grayscale-safe. A HALF/FULL clear firms the e-ink particles too
    //     hard for the X4 grayscale LUT to darken back, washing AA/image/sleep
    //     pages whitish. FAST avoids that (same trick as the image-blanking dance).
    //   - HALF: stronger ghost clear. Safe on X3 (1-bit panel, no grayscale
    //     image pass); on X4 may wash grayscale content whitish.
    //   - FULL: multi-cycle deep clean (deepCleanPanel), for image sticking that a
    //     single inversion cannot release — a black rule held at a fixed y for
    //     minutes, e.g. the themed header underline through an SD firmware write.
    //     Takes ~15s of visible black/white flashing and is the only way to clear
    //     burn that is already set, so it is a deliberate user action, not a default.
    if (SETTINGS.refreshScreenMode == CrossPointSettings::RSM_FULL) {
      unsigned long cleanMs = 0;
      {
        RenderLock lock;
        cleanMs = renderer.deepCleanPanel();
      }
      // On the SD log, not just serial: this is the one burn-in remedy the user can
      // trigger by hand, and without a trace here a later "the line came back" report
      // can't be told apart from "no clean was ever run".
      SdDebugLog::setEnabled(true);
      SdDebugLog::log("GFX", "deepclean manual cycles=3 ms=%lu", cleanMs);
    } else {
      const HalDisplay::RefreshMode clearMode = SETTINGS.refreshScreenMode == CrossPointSettings::RSM_HALF
                                                    ? HalDisplay::HALF_REFRESH
                                                    : HalDisplay::FAST_REFRESH;
      RenderLock lock;
      renderer.clearScreen();
      renderer.displayBuffer(clearMode);
    }
    activityManager.requestUpdate();
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  // Body complete: releases the slice hook's yield (see onEinkBusyWaitSlice).
  powerManager.noteMainLoopIteration();

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    const unsigned long idleMs = millis() - lastActivityTime;
    if (idleMs >= HalPowerManager::IDLE_LIGHT_SLEEP_MS) {
      // Idle: light-sleep between input polls instead of busy-delaying (same poll cadence).
      // Race-to-sleep: run the brief wake windows at normal clock, not LOW_POWER_FREQ.
      // The board's sleep-floor current is paid per-millisecond regardless of CPU
      // speed, so finishing the per-wake work ~16x faster and returning to sleep
      // costs less charge than stretching the window at 10 MHz (measured at 10 MHz:
      // 8.8 mA for 4.5 ms per wake). The downclock below only serves the pre-sleep
      // 100 Hz delay-poll phase. The lightSleep()-rejected fallback delay() then
      // also runs at normal clock, but that only happens when USB (externally
      // powered), WiFi, or a render Lock (full speed wanted anyway) is active.
      powerManager.setPowerSaving(false);
      if (gpio.isDebouncePending()) {
        // A raw button-state change is mid-debounce: commitment needs a second
        // matching sample, so poll again quickly instead of sleeping a slice —
        // a tap shorter than the 50 ms cadence would otherwise land in a single
        // sample and be dropped, and every press would commit a slice late.
        delayWallClock(10);
      } else if (!powerManager.lightSleep(gpio)) {
        // Light sleep declined = a render Lock, USB, or WiFi is active — the
        // chip is at full clock anyway, so poll at 100 Hz. A 50 ms cadence
        // here dropped sub-slice power taps (a press needs two samples >=5 ms
        // apart to commit), which made short-press sleep flaky during renders
        // — exactly when a render Lock forces this fallback.
        delayWallClock(10);
      }
    } else {
      // Response window after recent input: keep 100 Hz polling for snappy interaction,
      // but downclock once rapid-input bursts have settled — renders re-raise the clock
      // via HalPowerManager::Lock, so full speed only serves loop bookkeeping here
      if (idleMs >= HalPowerManager::IDLE_DOWNCLOCK_MS) {
        powerManager.setPowerSaving(true);
      }
      delayWallClock(10);
    }
  }
}
