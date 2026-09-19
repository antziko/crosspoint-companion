#include "SleepActivity.h"

#include <BitmapRenderUtils.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <SdDebugLog.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_random.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SleepImageRender.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {

const char* refreshModeName(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "FULL";
    case HalDisplay::HALF_REFRESH:
      return "HALF";
    case HalDisplay::SCRUB_REFRESH:
      return "SCRUB";
    default:
      return "FAST";
  }
}

// Every panel activation the sleep path makes goes through here, so the SD trace carries
// one line per activation: which waveform, and how long the controller actually held BUSY.
// The duration is the diagnostic -- a pass that returns far short of its waveform's cost
// never drove the panel, which is the difference between "the dose is too weak" and "the
// paint never reached the glass".
unsigned long timedPaint(const GfxRenderer& renderer, const HalDisplay::RefreshMode mode, const char* what) {
  const unsigned long startMs = millis();
  renderer.displayBuffer(mode);
  const unsigned long ms = millis() - startMs;
  SdDebugLog::log("SLP", "paint %s mode=%s ms=%lu", what, refreshModeName(mode), ms);
  return ms;
}

// Board and render-path identity, logged once per sleep. The controller is the load-bearing
// field: HALF is a charge scrub on the UltraChip parts and a single-pass absolute waveform on
// SSD1677, so the same firmware leaves a different amount of pigment packed on each, and the
// trace is unreadable without knowing which silicon produced it. The settings row matters for
// the same reason -- the cover filter alone decides whether the wallpaper takes the grayscale
// pipeline or the 1-bit halftone one (SleepImageRender.h).
void logSleepEntry(const GfxRenderer& renderer, const bool fromTimeout, const bool quickResume) {
  SdDebugLog::log("SLP", "entry board=%s ctrl=%u var=%02X spiHz=%lu fromReader=%d fromTimeout=%d quickResume=%d",
                  BoardConfig::ACTIVE.name, static_cast<unsigned>(BoardConfig::ACTIVE.displayController),
                  BoardConfig::ACTIVE.displayControllerVariant,
                  static_cast<unsigned long>(BoardConfig::ACTIVE.displaySpiHz), APP_STATE.lastSleepFromReader ? 1 : 0,
                  fromTimeout ? 1 : 0, quickResume ? 1 : 0);
  // inverted is in here because night mode decides how much of the panel the reader held BLACK,
  // which is the load on the clean; it was absent for three rounds of this investigation and its
  // absence sent one of them down the wrong pipeline.
  //
  // fadingFix is the other settings row that changes the panel's electrical history rather than
  // its picture: it is the `turnOffScreen` argument threaded through displayBuffer
  // (GfxRenderer.cpp:2122 -> FreeInkDisplay.cpp:602 -> Uc8279X4Driver.cpp:509), and with it OFF
  // powerOnIfNeeded() never sees a POF, so the panel's DC-DC stays energised from the first paint
  // after boot until deepSleep() -- a whole reading session of rails on a static image. Without
  // this field the trace cannot say which of the two regimes produced a capture.
  SdDebugLog::log("SLP", "cfg screen=%u filter=%u tone=%u dither=%u coverMode=%u gray=%d inverted=%u fadingFix=%u",
                  SETTINGS.sleepScreen, SETTINGS.sleepScreenCoverFilter, SETTINGS.wallpaperTone, SETTINGS.imageDither,
                  SETTINGS.sleepScreenCoverMode, sleepImageUsesGrayscale(renderer) ? 1 : 0,
                  static_cast<unsigned>(SETTINGS.screenInverted), static_cast<unsigned>(SETTINGS.fadingFix));

  // The frame about to be cleaned, and what the panel has been through since the last clean.
  // These are the measurements the timing lines cannot make:
  //   heldMs   how long this image has been on the glass. Image sticking is set by DWELL, and the
  //            ghost that prompted this carried its own status-bar clock showing it came from a
  //            page held ~13 minutes, not from the page shown 40 s before sleep.
  //   ink      black coverage of the LOGICAL framebuffer; panelBlack folds in night mode, which
  //            is what the panel actually held. A night-mode page is ~85% black, and that is the
  //            load the clean has to lift.
  //   paints   differential history since the previous deep clean. A session of FAST page turns
  //            leaves a different residue than one that was fully repainted.
  // Logged from here, before drawPopup(): that call paints over the framebuffer, and in night mode
  // it repaints the whole page at flipped polarity, so anything measured after it describes the
  // popup's frame and not the one the reader actually held.
  //   railsMs  how long the panel's analog domain was energised over that same span, and parks
  //            how many times the idle park stood it down. This is the DC-stress integral: with
  //            no park and no fadingFix it equals the whole session, which is the condition
  //            image retention is set by. parks=0 with a large railsMs means the park never
  //            fired -- check it before reading anything else into a capture.
  const int ink = renderer.frameInkPercent();
  SdDebugLog::log("SLP", "frame heldMs=%lu ink=%d%% panelBlack=%d%% paints fast=%u half=%u full=%u scrub=%u",
                  renderer.msSinceLastPaint(), ink, SETTINGS.screenInverted ? 100 - ink : ink,
                  static_cast<unsigned>(renderer.paintCount(HalDisplay::FAST_REFRESH)),
                  static_cast<unsigned>(renderer.paintCount(HalDisplay::HALF_REFRESH)),
                  static_cast<unsigned>(renderer.paintCount(HalDisplay::FULL_REFRESH)),
                  static_cast<unsigned>(renderer.paintCount(HalDisplay::SCRUB_REFRESH)));
  SdDebugLog::log("SLP", "rails ms=%lu parks=%u up=%d", renderer.railsMs(), static_cast<unsigned>(renderer.parkCount()),
                  renderer.panelRailsUp() ? 1 : 0);
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  // Sleep screens always use normal polarity. This activity paints directly from
  // onEnter, outside ActivityManager's per-render polarity resolution, so clear any
  // inversion left over from a night-mode reader render.
  display.setInverted(false);

  // Drop any wallpaper recorded for a previous sleep. Only renderCustomSleepScreen's
  // random folder pick re-sets it below; every other sleep screen (blank/cover/
  // quick-resume/fixed /sleep.bmp) leaves it empty so the on-wake review prompt does
  // not fire for an image that was never shown. Save only when it actually changes —
  // sleeps are infrequent, but SPIFFS erase cycles are finite.
  if (!APP_STATE.lastSleepImagePath.empty()) {
    APP_STATE.lastSleepImagePath.clear();
    APP_STATE.saveToFile();
  }

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  logSleepEntry(renderer, fromTimeout, renderQuickResume);

  if (renderQuickResume) {
    // No deepclean line follows for this path -- quick resume keeps the last screen, which a
    // panel wipe cannot coexist with. Its absence in the trace is the expected shape, not a
    // missing write.
    SdDebugLog::log("SLP", "path=quick-resume (deepclean skipped)");
    return renderLastScreenSleepScreen();
  }

  // Blank the framebuffer before the popup, and never draw the popup over whatever was on it.
  //
  // drawPopup() pushes the WHOLE framebuffer, which at this point still holds the screen the user
  // was on -- and setInverted(false) above has just flipped the output polarity, so on a night-mode
  // reader this painted the entire page inverted: a full-page, page-shaped image written to the
  // glass seconds before the clean. The clean's black phase then drives everything to black, where
  // the already-dark text takes an impulse with nowhere to go optically while the light ground
  // takes a real transition. That asymmetry is text-shaped and relaxes back out over the minutes
  // the panel sits unpowered, which is the X4 Pro's "clean at sleep, ghost after a while" report
  // (photo: the ghost is dark-on-light, the NEGATIVE of what night mode shows).
  //
  // Clearing first costs nothing: deepCleanPanel below leaves the framebuffer blank by contract
  // anyway, and every sleep screen re-renders from scratch after it.
  //
  // Cleared to the polarity the panel is ALREADY in, not always to white. setInverted(false) above
  // means a white clear would swing a night-mode panel from ~90% black to full white for the two
  // seconds this popup is up, then straight back to black for the clean's first phase -- a whole
  // extra full-panel reversal, visible as an extra flash, immediately before the panel is parked.
  // Clearing to black there makes the popup frame continuous with both the page before it and the
  // clean after it. drawPopup paints its own framed box, so it reads on either ground.
  const uint8_t entryGround = SETTINGS.screenInverted ? 0x00 : 0xFF;
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    renderer.clearScreen(entryGround);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    renderer.clearScreen(entryGround);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  // Ghost clear before the sleep image goes down. Sleep is the one moment where a clean is
  // affordable: the panel is about to change completely, the user is not interacting, and every
  // sleep screen below re-renders from a cleared framebuffer anyway — which is what deepCleanPanel
  // leaves behind. Runs after the popup so the flashing has an explanation on screen before it
  // starts; the popup is wiped by the clean and replaced by the sleep screen.
  //
  // The quick-resume path never reaches here: it returned above precisely because it keeps the
  // last screen, which a panel wipe cannot coexist with.
  //
  // One cycle. Three were tried on the X4 Pro's ghosting report and measurably did not help: the
  // ghost is absent when the wallpaper appears and emerges over the following minutes, so it forms
  // AFTER the clean, during the unpowered hold -- there is no image on the glass for a larger dose
  // to remove. Cleaning at every sleep is still worth its ~3 s; cleaning harder is not, and the
  // difference is ~6 s on every single sleep plus the panel wear.
  static constexpr uint8_t kSleepDeepCleanCycles = 1;
  const unsigned long cleanMs = renderer.deepCleanPanel(kSleepDeepCleanCycles);
  // Not force-enabled like the manual refresh's line: this runs on every sleep, and forcing an
  // SD write each time would cost more than the diagnostic is worth. Visible with SD Card
  // Logging on, which is when we are measuring anyway.
  SdDebugLog::log("SLP", "sleep deepclean cycles=%u ms=%lu", static_cast<unsigned>(kSleepDeepCleanCycles), cleanMs);

  // Hand the SD glyph arenas and the decompressor cache back before the sleep screens
  // run. The image paths below (custom bitmap, PNG wallpaper, book cover) each want a
  // large contiguous block for the decode, and a reader session leaves tens of KB of
  // resident glyph data that nothing after this point needs -- the two label strings on
  // the default screen re-fetch from SD at a cost that is invisible on the way into
  // sleep. No RenderLock: SleepActivity does not override render(), so the render task
  // has no paint of its own that could be walking these arenas (cf. the OPDS teardown
  // race, where it did).
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("SLP", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseCache();
    LOG_DBG("SLP", "Free heap before sleep screen render: %d bytes", ESP.getFreeHeap());
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    configureSleepBitmap(bitmap, renderer);  // dither target + halftone tone
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      SdDebugLog::log("SLP", "wallpaper=/sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      if (dir) dir.close();
      return;
    }
    file.close();
  }

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    // Pick a wallpaper without materialising the file list — a 1000+ image folder of
    // hash-named BMPs would exhaust the heap (vector<string>) and abort with bad_alloc
    // when sleep is entered from a low-memory reader state. Instead, two cheap directory
    // passes: count candidates, then walk to the chosen index. O(1) heap.
    //
    // Index == position in directory iteration order, which is stable for an unchanged
    // folder. Candidates are filtered by extension only; the single chosen file is the
    // only one actually opened and BMP-parsed (a corrupt pick falls through to default).
    char name[500];

    // Pass 1: count BMP candidates.
    uint16_t numFiles = 0;
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      if (dirFile.isDirectory()) {
        dirFile.close();
        continue;
      }
      dirFile.getName(name, sizeof(name));
      dirFile.close();
      if (name[0] == '.' || !FsHelpers::hasBmpExtension(std::string(name))) continue;
      if (++numFiles == UINT16_MAX) break;  // guard the counter
    }

    if (numFiles > 0) {
      // Hybrid selection:
      //  - Folder fits the shuffle-bag deck (<= SLEEP_DECK_MAX): exhaustive no-repeat
      //    pick over the persistent bitset — every image shown once before any repeat.
      //  - Larger: uniform random over ALL files (the fixed 512-bit deck cannot track
      //    them), accepting occasional repeats to cover every image without RAM cost.
      uint16_t pickIndex = 0;
      if (numFiles <= CrossPointState::SLEEP_DECK_MAX) {
        const uint16_t fileCount = numFiles;
        const uint16_t prevDeckSize = APP_STATE.sleepDeckSize;
        const uint16_t prevShownCount = APP_STATE.sleepDeckShownCount;
        const bool didReset = (prevDeckSize != fileCount || prevShownCount >= fileCount);
        if (didReset) APP_STATE.resetSleepDeck(fileCount);

        // Count images not yet shown this cycle, then pick the target-th of them.
        // Hardware TRNG (esp_random); Arduino random() is never seeded here.
        uint16_t eligible = 0;
        for (uint16_t i = 0; i < fileCount; i++) {
          if (!APP_STATE.isSleepShown(i)) eligible++;
        }
        if (eligible == 0) {
          pickIndex = static_cast<uint16_t>(esp_random() % fileCount);  // shouldn't happen; stay safe
        } else {
          const uint16_t target = static_cast<uint16_t>(esp_random() % eligible);
          for (uint16_t i = 0, seen = 0; i < fileCount; i++) {
            if (APP_STATE.isSleepShown(i)) continue;
            if (seen == target) {
              pickIndex = i;
              break;
            }
            seen++;
          }
        }
        APP_STATE.markSleepShown(pickIndex);
        APP_STATE.saveToFile();
        LOG_DBG("SLP", "deck files=%u prevSize=%u prevShown=%u reset=%d eligible=%u pick=%u", fileCount, prevDeckSize,
                prevShownCount, didReset ? 1 : 0, eligible, pickIndex);
      } else {
        // Too many for the deck — uniform random over the whole folder.
        pickIndex = static_cast<uint16_t>(esp_random() % numFiles);
        LOG_DBG("SLP", "deck bypass (files=%u > %u): uniform pick=%u", numFiles,
                (unsigned)CrossPointState::SLEEP_DECK_MAX, pickIndex);
      }

      // Pass 2: walk to the chosen candidate and render it.
      dir.rewindDirectory();
      uint16_t idx = 0;
      for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
        if (dirFile.isDirectory()) {
          dirFile.close();
          continue;
        }
        dirFile.getName(name, sizeof(name));
        if (name[0] == '.' || !FsHelpers::hasBmpExtension(std::string(name))) {
          dirFile.close();
          continue;
        }
        if (idx != pickIndex) {
          idx++;
          dirFile.close();
          continue;
        }
        // This is the pick. Build the path, then open+parse it specifically.
        const auto filename = std::string(sleepDir) + "/" + name;
        dirFile.close();
        HalFile randFile;
        if (Storage.openFileForRead("SLP", filename, randFile)) {
          LOG_DBG("SLP", "Randomly loading: %s", filename.c_str());
          Bitmap bitmap(randFile, true);
          configureSleepBitmap(bitmap, renderer);  // dither target + halftone tone
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            // Record the shown wallpaper so the on-wake review prompt can offer
            // keep/remove for it. Skipped implicitly for already-kept images (the
            // prompt ignores ".keep.bmp" picks on wake).
            APP_STATE.lastSleepImagePath = filename;
            APP_STATE.saveToFile();
            SdDebugLog::log("SLP", "wallpaper=%s", filename.c_str());
            renderBitmapSleepScreen(bitmap);
            randFile.close();
            dir.close();
            return;
          }
          randFile.close();
        }
        break;  // open/parse failed → fall through to the default screen
      }
    }
  }
  if (dir) dir.close();

  renderDefaultSleepScreen();
}

// Paint the finished sleep frame, then settle it with a second identical pass.
//
// The sleep frame is the one frame that has to hold for hours with the panel unpowered.
// A single pass parks the pigment short of fully packed, and across that hold it relaxes
// far enough for the previous screen's history to surface through it -- which is why the
// wallpaper is clean when it appears and the ghost grows afterwards. That timing is the
// tell: an under-cleaned panel would show the ghost immediately, and the deepCleanPanel in
// onEnter has already wiped it. Driving the same target a second time packs the pigment
// deeper, the same reason stock e-readers flash a screensaver more than once.
//
// A repeat, not a heavier waveform, because HALF is not the same primitive on every
// controller this board ships with: on the UltraChip batches it is the charge SCRUB (OLD
// plane = complement of the target, so every pixel transitions, Uc8279X4Driver.cpp:404-413)
// and a FULL there would white-seed and leave the white background parked untouched, while
// on SSD1677 both are absolute and FULL merely flashes longer. Repeating HALF is the one
// dose that is correct on all of them.
//
// X3 opts out: its HALF is already a forced three-pass resync (~3.2s), so it scrubs harder
// per pass and costs far too much to repeat.
void SleepActivity::paintSleepFrame() const {
  timedPaint(renderer, HalDisplay::HALF_REFRESH, "sleep-frame-1");
  if (!gpio.deviceIsX3()) timedPaint(renderer, HalDisplay::HALF_REFRESH, "sleep-frame-2");
}

// Sleep screens paint with the HALF refresh (stock parity): the OEM X4 firmware's only
// clean refresh in normal operation is the single-pass 0xD7 sequence, used for the sleep
// image. It never runs the multi-flash GC waveform (0xF7) that FULL_REFRESH selects
// (#2471's blinking complaint). paintSleepFrame() repeats it -- see there for why the dose
// is a second pass rather than a heavier waveform.
void SleepActivity::renderDefaultSleepScreen() const {
  SdDebugLog::log("SLP", "path=default");
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings.
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  paintSleepFrame();
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  const auto place = BitmapRenderUtils::centeredPlacement(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight,
                                                          sleepImageCrops());
  const int x = place.x;
  const int y = place.y;
  const float cropX = place.cropX;
  const float cropY = place.cropY;

  LOG_DBG("SLP", "drawing to %d x %d, crop %f x %f", x, y, cropX, cropY);

  // ONE panel activation, not two. There is no separate wipe pass: the paint below is
  // Half, and Half drives EVERY pixel to its target ignoring the previous frame -- it
  // seeds the OLD plane with the complement of the target on X4
  // (Uc8279X4Driver.cpp:289) and loads the WW==BW / WB==BB scrub bank on X3
  // (Uc8253X3Driver.cpp:188). Prior content cannot survive it, so a wipe before it only
  // adds a second flash.
  //
  // A Full wipe was in fact the WEAKER of the two: it seeds the OLD plane white, which
  // redraws only black-target pixels and leaves background ghost parked in WW (the
  // driver names this as the residual ghosting a white-seed Half once caused). Half is
  // also what the reader uses as its periodic ghost purge and its manual force-refresh.
  renderer.clearScreen();

  // X3's 4-level grayscale (gc) waveform washes out mid-tones a few seconds after the clean
  // BW frame is shown, so it keeps the dithered BW render instead. Same answer the dither
  // target above was chosen from -- the two cannot be decided separately.
  const bool hasGreyscale = bitmap.hasGreyscale() && sleepImageUsesGrayscale(renderer);
  SdDebugLog::log("SLP", "path=bitmap %dx%d at %d,%d crop=%d,%d bmpGray=%d gray=%d", bitmap.getWidth(),
                  bitmap.getHeight(), x, y, static_cast<int>(cropX * 100), static_cast<int>(cropY * 100),
                  bitmap.hasGreyscale() ? 1 : 0, hasGreyscale ? 1 : 0);

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (sleepImageInverts()) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    const unsigned long baseStartMs = millis();
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
    SdDebugLog::log("SLP", "paint gray-base mode=HALF ms=%lu", millis() - baseStartMs);
  } else {
    paintSleepFrame();
  }

  if (hasGreyscale) {
    const unsigned long overlayStartMs = millis();
    BitmapRenderUtils::applyGrayscaleOverlay(renderer, bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    SdDebugLog::log("SLP", "paint gray-overlay ms=%lu", millis() - overlayStartMs);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      SdDebugLog::log("SLP", "wallpaper=cover %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  if (gpio.deviceIsX3()) {
    // The controller still holds the displayed page, so its differential base
    // waveform can add the moon without a full-screen flash.
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    timedPaint(renderer, HalDisplay::HALF_REFRESH, "quick-resume");
  }
}

void SleepActivity::renderBlankSleepScreen() const {
  SdDebugLog::log("SLP", "path=blank");
  renderer.clearScreen();
  paintSleepFrame();
}
