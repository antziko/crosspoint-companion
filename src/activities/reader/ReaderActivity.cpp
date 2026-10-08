#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <optional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NtpBgState.h"
#include "util/LoanDue.h"

namespace {
// The boot-time background NTP sync (X4 only — gated on no hardware RTC, see
// maybeStartBackgroundNtpSync()) holds the WiFi stack up for up to ~28s, which
// fragments the heap below the 32KB contiguous block an EPUB inflate needs.
// Opening a book during that window OOM-aborts the device. Give a fast sync a
// short grace to finish on its own, then cancel it and wait for the radio
// teardown so the book opens on a recovered heap. Back skips the grace at once.
// No-op (single false check) when no bg sync is running — always the case on X3.
void waitOutBackgroundNtpSync(GfxRenderer& renderer, MappedInputManager& mappedInput) {
  if (!NtpBg::active) return;

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int cy = renderer.getScreenHeight() / 2;
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, cy - lineHeight, tr(STR_CLOCK_SYNCING));
  renderer.drawCenteredText(SMALL_FONT_ID, cy + lineHeight, tr(STR_CLOCK_SYNC_SKIP_HINT));
  renderer.displayBuffer();

  constexpr uint32_t graceMs = 5000;  // let a typical 2-3s sync land before opening
  const uint32_t start = millis();
  while (NtpBg::active) {
    mappedInput.update();
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || (millis() - start) >= graceMs) {
      // Request teardown; the task aborts its SNTP wait (within ~100ms), drops the
      // radio, and clears NtpBg::active, which ends this loop on a recovered heap.
      NtpBg::cancel = true;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
}  // namespace

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isImageFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path);
}

int ReaderActivity::initialRefreshCountdown() const {
  // Surgical first-paint scrub: only entries that land the reader over a
  // full-screen frame a fast diff / gentle X3 B/W reinforcement can't clear ask
  // for it -- cold boot (boot logo), wake-from-sleep (sleep image), and KOReader
  // sync return (the sync / "Progress found" screen). Those callers pass
  // allowFastInitialRefresh=false; FORCE_FULL then keeps the first render on the
  // HALF scrub path even when periodic maintenance is set to B/W reinforcement
  // (displayWithRefreshCycle excludes FORCE_FULL from reinforce). Ordinary
  // re-entries (book open, end-of-book "open next") default to fast so they don't
  // flash on every open.
  if (!allowFastInitialRefresh) return CrossPointSettings::REFRESH_COUNTDOWN_FORCE_FULL;

  const int refreshFrequency = SETTINGS.getEffectiveRefreshFrequency();
  return refreshFrequency > 1 ? refreshFrequency : 2;
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    // The popup replaces the restored Quick Resume frame, so the reader must clean it.
    allowFastInitialRefresh = false;
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
  bool loaded;
  {
    // Lend the framebuffer's 48 KB to the container parse (expat + spine/TOC
    // build). The popup just displayed stays on the panel; whichever reader
    // activity follows redraws the full screen anyway.
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (loaded) {
    return epub;
  }

  loadProtectionError = epub->getProtectionError();
  LOG_ERR("READER", "Failed to load epub%s%s", loadProtectionError.empty() ? "" : ": ", loadProtectionError.c_str());
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  auto epubReader =
      makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(epub), initialRefreshCountdown());
  if (!epubReader) {
    LOG_ERR("READER", "OOM: EpubReaderActivity; returning home");
    activityManager.goHome();
    return;
  }
  activityManager.replaceActivity(std::move(epubReader));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  auto viewer = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, path);
  if (!viewer) {
    LOG_ERR("READER", "OOM: BmpViewerActivity; returning home");
    activityManager.goHome();
    return;
  }
  activityManager.replaceActivity(std::move(viewer));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  auto xtcReader =
      makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(xtc), initialRefreshCountdown());
  if (!xtcReader) {
    LOG_ERR("READER", "OOM: XtcReaderActivity; returning home");
    activityManager.goHome();
    return;
  }
  activityManager.replaceActivity(std::move(xtcReader));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  // Don't load a book while boot-time background NTP holds WiFi up — the
  // fragmented heap can't fit the EPUB inflate window and the open OOM-aborts.
  waitOutBackgroundNtpSync(renderer, mappedInput);

  sdFontSystem.ensureLoaded(renderer);

  currentBookPath = initialBookPath;
  if (isImageFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else {
    // .epub, and .txt/.md, which Epub converts into XHTML parts on first open.
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      if (!loadProtectionError.empty()) {
        showProtectionError();
        return;
      }
      onGoBack();
      return;
    }
    if (loandue::dueSoon(epub->getLoanExpiresAt())) {
      showLoanDueReminder(std::move(epub));
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

ReaderActivity::ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                               const bool allowFastInitialRefresh)
    : Activity("Reader", renderer, mappedInput),
      initialBookPath(std::move(initialBookPath)),
      allowFastInitialRefresh(allowFastInitialRefresh) {}

ReaderActivity::~ReaderActivity() = default;

void ReaderActivity::onGoBack() { finish(); }

void ReaderActivity::showLoanDueReminder(std::unique_ptr<Epub> epub) {
  char message[64];
  if (!loandue::describe(epub->getLoanExpiresAt(), message, sizeof(message))) {
    onGoToEpubReader(std::move(epub));
    return;
  }
  pendingEpub = std::move(epub);
  const bool shown = startActivityForResultNoThrow<ConfirmationActivity>(
      [this](const ActivityResult&) { onGoToEpubReader(std::move(pendingEpub)); }, renderer, mappedInput, "", message,
      "", tr(STR_OK_BUTTON));
  if (!shown) onGoToEpubReader(std::move(pendingEpub));
}

// Exact error strings are set by openProtectedBook (lib/Epub/ContentProtection.cpp).
void ReaderActivity::showProtectionError() {
  StrId msg = StrId::STR_DRM_PROTECTED_FILE;
  bool offerSync = false;
  if (loadProtectionError == "access expired") {
    msg = StrId::STR_LOAN_EXPIRED;
  } else if (loadProtectionError == "loan date unverified") {
    msg = StrId::STR_LOAN_TIME_UNVERIFIED;
    offerSync = true;
  }
  const bool shown = startActivityForResultNoThrow<ConfirmationActivity>(
      [this, offerSync](const ActivityResult& result) {
        if (offerSync && !result.isCancelled) {
          beginLoanTimeSync();
          return;
        }
        onGoBack();
      },
      renderer, mappedInput, "", I18N.get(msg), offerSync ? tr(STR_OK_BUTTON) : "",
      I18N.get(offerSync ? StrId::STR_CLOCK_SYNC_NOW : StrId::STR_OK_BUTTON));
  if (!shown) onGoBack();
}

void ReaderActivity::beginLoanTimeSync() {
  const bool shown = startActivityForResultNoThrow<WifiSelectionActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
          onGoBack();
          return;
        }
        GUI.drawPopup(renderer, tr(STR_SYNCING_TIME));
        const bool synced = trustedtime::syncNow(5000);
        WiFi.disconnect(false);
        delay(30);
        if (!synced) {
          loadProtectionError = "loan date unverified";
          showProtectionError();
          return;
        }
        APP_STATE.openEpubPath = initialBookPath;
        APP_STATE.saveToFile();
        // Reboot straight back into this book on a clean heap (no-op on touch boards,
        // which relaunch in place).
        silentRestartToReader();
        activityManager.goToReader(initialBookPath);
      },
      renderer, mappedInput);
  if (!shown) onGoBack();
}
