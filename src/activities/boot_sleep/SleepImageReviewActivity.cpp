#include "SleepImageReviewActivity.h"

#include <Bitmap.h>
#include <BitmapRenderUtils.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SleepImageRender.h"
#include "components/UITheme.h"
#include "fontIds.h"

SleepImageReviewActivity::SleepImageReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::string imagePath, bool resumeToReader, std::string readerPath)
    : Activity("SleepImageReview", renderer, mappedInput),
      imagePath(std::move(imagePath)),
      resumeToReader(resumeToReader),
      readerPath(std::move(readerPath)) {}

void SleepImageReviewActivity::onEnter() {
  // Sized to one line of the hint font. Laid out before the first render so the bar can
  // be drawn over the wallpaper and hit-tested against the same boxes.
  actionBar_.layout(renderer, mappedInput.hasTouch(), UI_10_FONT_ID, 3);
  Activity::onEnter();
  renderImage();
}

// Re-render path (manual refresh / requestUpdate): the framebuffer was cleared to
// white before this runs, so re-decode and redraw the current bitmap.
void SleepImageReviewActivity::render(RenderLock&&) { renderImage(); }

void SleepImageReviewActivity::renderImage() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  HalFile file;
  if (!Storage.openFileForRead("SLPR", imagePath, file)) {
    // Image vanished between boot check and here — nothing to review, move on.
    finishToDestination();
    return;
  }

  Bitmap bitmap(file, true);
  configureSleepBitmap(bitmap, renderer);  // dither target + halftone tone

  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    finishToDestination();
    return;
  }

  // Full screen, with the action bar drawn OVER the image (the arrangement the BMP viewer
  // already uses). Reserving a strip for the bar instead would shrink the viewport, and any
  // scale the sleep render did not use wrecks the picture: the bitmap is dithered in SOURCE
  // pixels during decode (Bitmap.cpp packPixel) and drawBitmap resamples by nearest
  // neighbour, so a wallpaper authored at panel size goes from "not scaled at all" to a
  // ~0.92 decimation of the threshold field -- which prints as a grid. Same screen size as
  // the sleep render means the same scale, and therefore the same pixels.
  const auto place = BitmapRenderUtils::centeredPlacement(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight,
                                                          sleepImageCrops());
  const int x = place.x;
  const int y = place.y;

  // Back = Skip, Confirm = Keep, Right = Remove. Keep/Skip both continue; only
  // Remove and Keep change the file (see loop()).
  const auto labels = mappedInput.mapLabels(tr(STR_SKIP), tr(STR_KEEP), "", tr(STR_REMOVE));

  // The same answer the sleep render used, so this screen shows the image the user actually
  // woke up to -- the cover filter suppresses the gray planes, and reviewing a filtered
  // wallpaper through the unfiltered pipeline would show a different picture. Crop and
  // inversion are matched below for the same reason.
  const bool hasGreyscale = bitmap.hasGreyscale() && sleepImageUsesGrayscale(renderer);

  // Same single-activation recipe as SleepActivity::renderBitmapSleepScreen. This screen
  // shows the very image the panel is still retaining from sleep, so the paint must be
  // HALF, never FAST: FAST is the turbo differential driven off the stale plane
  // (Uc8253X3Driver.cpp:191-206), while HALF loads the `_half` scrub bank on X3 and seeds
  // the OLD plane with the complement of the target on X4 -- either way every pixel is
  // driven to its target regardless of what the panel was holding. That is what erases the
  // retained frame, so no separate wipe pass is needed ahead of it.
  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, place.cropX, place.cropY);
  // Before the controls below, not after: the inversion covers the whole framebuffer, so
  // running it over the bar and the hints would leave white-on-black labels. Both draw
  // their own ground, so they stay legible on the inverted image.
  if (sleepImageInverts()) {
    renderer.invertScreen();
  }
  // Skip / Keep / Remove, on top of the wallpaper. Both the hint boxes and the bar paint
  // their own ground, so they stay readable over a photo -- but the grayscale pass below
  // repaints the gray planes across the whole screen, so on a grayscale render the photo's
  // gray still tints them. That is the price of showing the wallpaper at its true scale.
  if (actionBar_.active()) {
    const char* barLabels[] = {tr(STR_SKIP), tr(STR_KEEP), tr(STR_REMOVE)};
    actionBar_.draw(renderer, UI_10_FONT_ID, barLabels, /*primaryIndex=*/1);
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (hasGreyscale) {
    // Must be displayGrayscaleBase, not displayBuffer: the gray nudge LUT applied by
    // applyGrayscaleOverlay below is calibrated against the pixel state this base leaves
    // behind (see the same call in SleepActivity).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (hasGreyscale) {
    // Same crop as the BW base above: an uncropped gray pass would land its planes on
    // different pixels than the image they are shading.
    BitmapRenderUtils::applyGrayscaleOverlay(renderer, bitmap, x, y, pageWidth, pageHeight, place.cropX, place.cropY);
  }

  file.close();
}

void SleepImageReviewActivity::doKeep() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  // "<dir>/<name>.bmp" -> "<dir>/<name>.keep.bmp". hasBmpExtension still matches, so
  // the image stays in the sleep rotation; isKeptSleepImage() skips it on future wakes.
  std::string kept = imagePath;
  if (FsHelpers::hasBmpExtension(imagePath)) {
    kept = imagePath.substr(0, imagePath.length() - 4) + ".keep.bmp";
  }
  const bool ok = (kept != imagePath) && Storage.rename(imagePath.c_str(), kept.c_str());
  // The sleep deck is indexed by directory position; a rename can reorder iteration and
  // desync the "already shown" bitset. Invalidate it so the next sleep starts a fresh
  // cycle. Zero allocation — resetSleepDeck() only clears the existing 64-byte bitset.
  if (ok) APP_STATE.resetSleepDeck(0);
  GUI.drawPopup(renderer, ok ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(800);
  finishToDestination();
}

void SleepImageReviewActivity::doRemove() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  // "<dir>/<name>" -> "<dir>/.<name>". The leading dot excludes it from the sleep
  // picker (SleepActivity skips name[0]=='.') without deleting anything — reversible.
  const std::string dir = FsHelpers::extractFolderPath(imagePath);
  const size_t lastSlash = imagePath.find_last_of('/');
  const std::string name = (lastSlash != std::string::npos) ? imagePath.substr(lastSlash + 1) : imagePath;
  std::string hidden = dir;
  if (!hidden.empty() && hidden.back() != '/') hidden += "/";
  hidden += "." + name;

  const bool ok = Storage.rename(imagePath.c_str(), hidden.c_str());
  // Removal drops a file from the rotation; the next sleep would reset the deck anyway
  // (file count changed), but invalidate explicitly so the positional bitset never maps
  // to the wrong files in between. Zero allocation (clears the existing bitset).
  if (ok) APP_STATE.resetSleepDeck(0);
  GUI.drawPopup(renderer, ok ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(800);
  finishToDestination();
}

void SleepImageReviewActivity::finishToDestination() {
  // One-shot: clear the recorded image so the prompt never re-fires for it.
  APP_STATE.lastSleepImagePath.clear();
  APP_STATE.saveToFile();

  if (resumeToReader && !readerPath.empty()) {
    // Wake-from-sleep: the panel still holds the sleep image, which a fast first
    // paint can't clear. Force the initial HALF scrub (see
    // ReaderActivity::initialRefreshCountdown).
    activityManager.goToReader(readerPath, /*allowFastInitialRefresh=*/false);
  } else {
    activityManager.goHome();
  }
}

void SleepImageReviewActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepImageReviewActivity::loop() {
  Activity::loop();

  int tapX = 0;
  int tapY = 0;
  if (mappedInput.wasScreenTapped(tapX, tapY)) {
    switch (actionBar_.hitAt(tapX, tapY)) {
      case 0:
        finishToDestination();  // Skip
        return;
      case 1:
        doKeep();
        return;
      case 2:
        doRemove();
        return;
      default:
        break;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishToDestination();  // Skip: leave the image unchanged
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doKeep();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    doRemove();
    return;
  }
}
