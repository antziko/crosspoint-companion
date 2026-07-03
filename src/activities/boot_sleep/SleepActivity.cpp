#include "SleepActivity.h"

#include <BitmapRenderUtils.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_random.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

void SleepActivity::onEnter() {
  Activity::onEnter();

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

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
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
    bitmap.setOneBitDither(renderer.isX3());          // X3: 1-bit halftone, avoids wash-out
    bitmap.setImageDitherMode(SETTINGS.imageDither);  // blue/bayer/error-diffusion (X4)
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
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
          bitmap.setOneBitDither(renderer.isX3());          // X3: 1-bit halftone, avoids wash-out
          bitmap.setImageDitherMode(SETTINGS.imageDither);  // blue/bayer/error-diffusion (X4)
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            // Record the shown wallpaper so the on-wake review prompt can offer
            // keep/remove for it. Skipped implicitly for already-kept images (the
            // prompt ignores ".keep.bmp" picks on wake).
            APP_STATE.lastSleepImagePath = filename;
            APP_STATE.saveToFile();
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

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);

  // Wipe ghosting from the previously displayed screen before drawing the
  // wallpaper. Clear to white first, then a FULL refresh (full black/white
  // flash) to completely clear prior content before showing the image.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  renderer.clearScreen();

  // X3's 4-level grayscale (gc) waveform washes out mid-tones a few seconds
  // after the clean BW frame is shown, so skip the grayscale overlay there and
  // keep the dithered BW render (which already reproduces the image well).
  const bool hasGreyscale = bitmap.hasGreyscale() && !renderer.isX3() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    // OEM grayscale pipeline base: use a full sleep-screen paint so the panel
    // enters deep sleep from a clean B/W baseline before the gray nudge refresh.
    renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }

  if (hasGreyscale) {
    BitmapRenderUtils::applyGrayscaleOverlay(renderer, bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
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
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
