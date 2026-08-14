#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <optional>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NtpBgState.h"

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

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

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

  const int refreshFrequency = SETTINGS.getRefreshFrequency();
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

  LOG_ERR("READER", "Failed to load epub");
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

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint"));
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
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

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  auto txtReader =
      makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(txt), initialRefreshCountdown());
  if (!txtReader) {
    LOG_ERR("READER", "OOM: TxtReaderActivity; returning home");
    activityManager.goHome();
    return;
  }
  activityManager.replaceActivity(std::move(txtReader));
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
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
