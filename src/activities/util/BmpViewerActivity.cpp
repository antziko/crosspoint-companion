#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
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

  // One pass: track the largest .bmp name strictly before the current file and the
  // smallest strictly after it. Bounded RAM (two strings) for any folder size.
  char name[500];
  size_t scanned = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        const std::string fname(name);
        if (fname.length() >= 4 && fname.substr(fname.length() - 4) == ".bmp" && fname != fileName) {
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

void BmpViewerActivity::renderImage() {
  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // "Loading" popup. No incremental progress fill: each fillPopupProgress() did a
  // ~637ms FAST e-ink refresh, and the bar only covered the fast header-parse (the
  // slow part is the image refresh chain below, which it never tracked).
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);
    bitmap.setOneBitDither(renderer.isX3());  // X3: 1-bit halftone, full tonal detail
    bitmap.setImageDitherMode(SETTINGS.imageDither);  // blue/bayer/error-diffusion (X4)

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

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
      // and the dithered grays were invisible.
      const bool hasGreyscale = bitmap.hasGreyscale() && !renderer.isX3();

      // Wipe ghosting before drawing the image with a single mild HALF refresh
      // (not FULL's black-white-black-white flash).
      renderer.clearScreen();
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);

      renderer.clearScreen();
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
      // Draw UI hints on the base (BW) layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(hasGreyscale ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

      if (hasGreyscale) {
        // Overlay the 4-level grayscale planes (LSB then MSB), then drive the
        // panel with the combined gray frame — same sequence as the sleep cover.
        bitmap.rewindToData();
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
        renderer.copyGrayscaleLsbBuffers();

        bitmap.rewindToData();
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
        renderer.copyGrayscaleMsbBuffers();

        renderer.displayGrayBuffer();
        renderer.setRenderMode(GfxRenderer::BW);
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid BMP File");
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Could not open file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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
    } else {
      doSetSleepCover();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (!prevName.empty()) {
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + prevName;
      onEnter();  // recomputes siblings for the new current image
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (!nextName.empty()) {
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + nextName;
      onEnter();
    }
    return;
  }
}