#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <BitmapRenderUtils.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "activities/boot_sleep/SleepImageRender.h"
#include "components/UITheme.h"
#include "fontIds.h"

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

namespace {
// Strict total order over names: natural order with a byte-compare tiebreak (so distinct
// names are never "equal"). Matches FileBrowser's ordering.
bool bmpNameLess(const std::string& a, const std::string& b) {
  if (FsHelpers::naturalFileLess(a, b)) return true;
  if (FsHelpers::naturalFileLess(b, a)) return false;
  return a < b;
}
}  // namespace

void BmpViewerActivity::computeSiblings() {
  prevName.clear();
  nextName.clear();
  if (filePath.empty()) return;

  const std::string dirPath = FsHelpers::extractFolderPath(filePath);
  const size_t lastSlash = filePath.find_last_of('/');
  const std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  // One pass: track the largest image name strictly before the current file and the
  // smallest strictly after it. Bounded RAM (two strings) for any folder size.
  char name[500];
  size_t scanned = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        const std::string fname(name);
        const bool isImage = FsHelpers::hasBmpExtension(fname) || FsHelpers::hasPngExtension(fname);
        if (isImage && fname != fileName) {
          if (bmpNameLess(fname, fileName)) {
            if (prevName.empty() || bmpNameLess(prevName, fname)) prevName = fname;  // largest below
          } else if (bmpNameLess(fileName, fname)) {
            if (nextName.empty() || bmpNameLess(fname, nextName)) nextName = fname;  // smallest above
          }
        }
      }
    }
    file.close();
    if ((++scanned % 64) == 0) vTaskDelay(1);  // watchdog guard on huge folders
  }
  dir.close();
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (!filePath.empty()) {
    computeSiblings();
  }

  renderImage();
}

// Re-render path (manual refresh / requestUpdate). The framebuffer was cleared
// to white before this runs, so re-decode and redraw the current bitmap.
void BmpViewerActivity::render(RenderLock&&) { renderImage(); }

bool BmpViewerActivity::canSetSleepCover() const { return FsHelpers::hasBmpExtension(filePath); }

bool BmpViewerActivity::renderPngImage(const int pageWidth, const int pageHeight) {
  ImageDimensions dims{};
  if (!PngToFramebufferConverter::getDimensionsStatic(filePath, dims)) return false;
  if (dims.width <= 0 || dims.height <= 0) return false;

  // Fit inside the screen, never upscale: a small PNG stays its own size rather than
  // being blown up and re-dithered.
  const float fit =
      std::min({static_cast<float>(pageWidth) / dims.width, static_cast<float>(pageHeight) / dims.height, 1.0f});
  const int width = std::min(pageWidth, static_cast<int>(dims.width * fit));
  const int height = std::min(pageHeight, static_cast<int>(dims.height * fit));
  if (width <= 0 || height <= 0) return false;

  RenderConfig config;
  config.x = (pageWidth - width) / 2;
  config.y = (pageHeight - height) / 2;
  config.maxWidth = width;
  config.maxHeight = height;
  // Always dither to a 1-bit halftone here. DirectPixelWriter latches the render mode at
  // init, so one decodeToFramebuffer() call fills exactly one plane; 4-level grayscale
  // would need a decode per plane (BW/LSB/MSB) like BitmapRenderUtils does for BMP, and a
  // PNG decode is far too slow to run three times. A halftone carries full tonal detail in
  // the single BW pass instead of dropping the grays and rendering dark.
  config.oneBitDither = true;
  config.ditherBlueNoise = renderer.imageDitherBlueNoise();  // Display > Image Dither
  config.useExactDimensions = true;                          // dimensions already fitted above

  PngToFramebufferConverter converter;
  return converter.decodeToFramebuffer(filePath, renderer, config);
}

void BmpViewerActivity::renderImage() {
  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // "Loading" popup. No incremental progress fill: each fillPopupProgress() did a
  // ~637ms FAST e-ink refresh, and the bar only covered the fast header-parse (the
  // slow part is the image refresh chain below, which it never tracked).
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  // PNG goes through the shared converter rather than Bitmap. Handled before the open
  // below so a .png never reaches the BMP header parser.
  if (FsHelpers::hasPngExtension(filePath)) {
    const bool hasPrevious = !prevName.empty();
    const bool hasNext = !nextName.empty();
    // A PNG can never become the cover (see canSetSleepCover), but an existing cover can
    // still be cleared from here, so the Confirm slot stays useful.
    coverExists = Storage.exists("/sleep.bmp");
    const char* confirmLabel = coverExists ? tr(STR_CLEAR_BUTTON) : "";

    // Wipe ghosting with the same single mild HALF refresh the BMP path uses.
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);

    renderer.clearScreen();
    if (renderPngImage(pageWidth, pageHeight)) {
      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), confirmLabel, (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      layoutActionBar(hasPrevious, hasNext, confirmLabel);
      actionBar_.draw(renderer, UI_10_FONT_ID, barLabels_);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      layoutActionBar(false, false, nullptr);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }

  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);
    // A BMP is a wallpaper candidate (see canSetSleepCover), so preview it through the
    // wallpaper pipeline -- dither target, three-tone quantization, Wallpaper Tone, crop
    // and the inversion filter all included. What is on screen here is what the sleep
    // screen will show, so choosing a cover is not a guess.
    configureSleepBitmap(bitmap, renderer);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      const auto place = BitmapRenderUtils::centeredPlacement(bitmap.getWidth(), bitmap.getHeight(), pageWidth,
                                                              pageHeight, sleepImageCrops());
      const int x = place.x;
      const int y = place.y;

      // 4. Prepare Rendering
      bool hasPrevious = !prevName.empty();
      bool hasNext = !nextName.empty();

      // If a sleep cover already exists, the Confirm button clears it (so the
      // sleep screen can resume randomizing from the folder); otherwise it sets
      // the current image as the cover.
      coverExists = Storage.exists("/sleep.bmp");
      const char* confirmLabel = coverExists ? tr(STR_CLEAR_BUTTON) : tr(STR_SET_SLEEP_COVER);
      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), confirmLabel, (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      // X4 (4-level grayscale) needs the multi-pass grayscale render to actually
      // show grays; X3 produces a 1-bit halftone (0/3) so a single BW pass is
      // correct. Without this, X4 BMPs showed only the 1-bit BW plane — too dark,
      // and the dithered grays were invisible. A cover filter suppresses the gray
      // planes, which is why the answer comes from the wallpaper pipeline and not
      // from the board alone.
      const bool hasGreyscale = bitmap.hasGreyscale() && sleepImageUsesGrayscale(renderer);

      // Wipe ghosting before drawing the image with a single mild HALF refresh
      // (not FULL's black-white-black-white flash).
      renderer.clearScreen();
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);

      renderer.clearScreen();
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, place.cropX, place.cropY);
      // Before the hints below, not after: the inversion covers the whole framebuffer, so
      // running it over them would leave white-on-black labels. Both the hint boxes and
      // the action bar draw their own ground, so they stay legible on the inverted image.
      if (sleepImageInverts()) {
        renderer.invertScreen();
      }
      // Draw UI hints on the base (BW) layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      layoutActionBar(hasPrevious, hasNext, confirmLabel);
      actionBar_.draw(renderer, UI_10_FONT_ID, barLabels_);
      renderer.displayBuffer(hasGreyscale ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

      if (hasGreyscale) {
        // Overlay the 4-level grayscale planes, then drive the panel with the
        // combined gray frame — same sequence as the sleep cover.
        BitmapRenderUtils::applyGrayscaleOverlay(renderer, bitmap, x, y, pageWidth, pageHeight, place.cropX,
                                                 place.cropY);
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_BMP_FILE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      layoutActionBar(false, false, nullptr);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    layoutActionBar(false, false, nullptr);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  bool success = false;
  HalFile inFile, outFile;
  if (Storage.openFileForRead("BMP", filePath, inFile)) {
    if (Storage.openFileForWrite("BMP", "/sleep.bmp", outFile)) {
      // 2KB copy buffer on the heap (was on the stack); rendering/UI activities
      // share a modest task stack.
      constexpr size_t COPY_BUF_SIZE = 2048;
      auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUF_SIZE);
      if (buffer) {
        int bytesRead;
        success = true;
        while ((bytesRead = inFile.read(buffer.get(), COPY_BUF_SIZE)) > 0) {
          if (outFile.write(buffer.get(), bytesRead) != bytesRead) {
            success = false;
            break;
          }
        }
      }
      outFile.close();
    }
    inFile.close();
  }

  if (success) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::doClearSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  // Already gone counts as success; the sleep screen falls back to the random
  // folder once /sleep.bmp is absent (SleepActivity checks it first).
  const bool removed = !Storage.exists("/sleep.bmp") || Storage.remove("/sleep.bmp");

  GUI.drawPopup(renderer, removed ? tr(STR_DONE) : tr(STR_FAILED_LOWER));
  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (coverExists) {
      doClearSleepCover();
    } else if (canSetSleepCover()) {
      doSetSleepCover();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    showSibling(prevName);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    showSibling(nextName);
    return;
  }

  int tx = 0;
  int ty = 0;
  if (actionBar_.active() && mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = actionBar_.hitAt(tx, ty);
    if (hit < 0) return;
    switch (barActions_[hit]) {
      case BarAction::Previous:
        showSibling(prevName);
        break;
      case BarAction::Next:
        showSibling(nextName);
        break;
      case BarAction::Cover:
        if (coverExists) {
          doClearSleepCover();
        } else if (canSetSleepCover()) {
          doSetSleepCover();
        }
        break;
    }
  }
}

void BmpViewerActivity::showSibling(const std::string& name) {
  if (name.empty()) return;
  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  if (dirPath.back() != '/') dirPath += "/";
  filePath = dirPath + name;
  onEnter();  // recomputes siblings for the new current image
}

void BmpViewerActivity::layoutActionBar(const bool hasPrevious, const bool hasNext, const char* coverLabel) {
  barCount_ = 0;
  if (mappedInput.hasTouch()) {
    if (hasPrevious) {
      barLabels_[barCount_] = "<";
      barActions_[barCount_++] = BarAction::Previous;
    }
    if (coverLabel != nullptr && coverLabel[0] != '\0') {
      barLabels_[barCount_] = coverLabel;
      barActions_[barCount_++] = BarAction::Cover;
    }
    if (hasNext) {
      barLabels_[barCount_] = ">";
      barActions_[barCount_++] = BarAction::Next;
    }
  }
  actionBar_.layout(renderer, mappedInput.hasTouch(), UI_10_FONT_ID, barCount_);
}