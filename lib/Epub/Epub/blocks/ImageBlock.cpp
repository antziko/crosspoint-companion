#include "ImageBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <Serialization.h>
#include <esp_heap_caps.h>

#include <cstdlib>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath, bool oneBit, bool blueNoise) {
  // Replace extension with the pixel-cache suffix. Different render paths
  // produce different pixel data for the same image, so each uses a distinct
  // suffix to avoid reading another's cache. The number is bumped whenever the
  // dither/tone math changes so stale caches regenerate:
  //   .px13n = X4 4-level, blue-noise field  (bumped: grey-text-on-dark DarkText
  //   .px13b = X4 4-level, 8x8 Bayer field    tone curve + grey-text detection)
  //   .px7n  = X3 / X4-AA-off 1-bit, blue-noise halftone (JPEG box-average)
  //   .px7b  = X3 / X4-AA-off 1-bit, Bayer halftone
  // Switching the Display > Image Dither setting therefore swaps cache files
  // rather than serving stale pixels. (Orphaned older caches stay on the card;
  // clear .crosspoint/ to reclaim that space.)
  const char* suffix = oneBit ? (blueNoise ? ".px7n" : ".px7b") : (blueNoise ? ".px13n" : ".px13b");
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + suffix;
  }
  return imagePath + suffix;
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read and render row by row to minimize memory usage
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  uint8_t* rowBuffer = (uint8_t*)malloc(bytesPerRow);
  if (!rowBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < cachedHeight; row++) {
    if (cacheFile.read(rowBuffer, bytesPerRow) != bytesPerRow) {
      LOG_ERR("IMG", "Cache read error at row %d", row);
      free(rowBuffer);
      return false;
    }

    const int destY = y + row;
    pw.beginRow(destY);
    for (int col = 0; col < cachedWidth; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(rowBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache(const GfxRenderer& renderer) const {
  const auto cachePath = getCachePath(imagePath, renderer.oneBitImages(), renderer.imageDitherBlueNoise());
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode(const GfxRenderer& renderer) const {
  return !imageFailedThisSession(imagePath) && !hasValidCache(renderer);
}

void ImageBlock::clearSessionRenderFailures() { failedImageCount = 0; }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The scan/prewarm pass only measures text to warm the font cache — it draws
  // nothing to the framebuffer. Decoding the image here is pure waste, and for a
  // large JPEG it costs seconds. The real BW + grayscale passes draw the image.
  if (renderer.isFontCacheScanning()) {
    return;
  }

  // A prior pass already failed to decode this image (too large / OOM). Don't
  // re-attempt it on every BW + grayscale-strip pass — that re-fails each time,
  // hanging the page for tens of seconds and fragmenting the heap.
  if (decodeFailed) {
    return;
  }

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  // X3 renders images as a 1-bit halftone written during the BW pass. The
  // grayscale (text-AA) passes re-render the page in GRAYSCALE_LSB/MSB modes;
  // a 1-bit image contributes nothing to those planes and the gc bb cell
  // preserves the halftone, so skip the (costly) image redraw entirely there.
  const bool oneBit = renderer.oneBitImages();  // X3 always; X4 when text AA is off
  const bool blueNoise = renderer.imageDitherBlueNoise();
  if (oneBit && renderer.getRenderMode() != GfxRenderer::BW) {
    return;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    SdDebugLog::log("IMG", "bad pos (%d,%d) %dx%d screen %dx%d %s", x, y, width, height, screenWidth, screenHeight,
                    imagePath.c_str());
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisSession(imagePath)) {
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath, oneBit, blueNoise);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    SdDebugLog::log("IMG", "file not found %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    SdDebugLog::log("IMG", "file empty %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());
  // X3 untethered: capture the decode attempt + heap shape so a silent image
  // failure leaves a trace. `largest` separates fragmentation from exhaustion.
  SdDebugLog::log("IMG", "decode start %s %dx%d 1bit=%d size=%u free=%u largest=%u", imagePath.c_str(), width, height,
                  oneBit ? 1 : 0, (unsigned)fileSize, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.oneBitDither = oneBit;          // X3: 1-bit halftone instead of 4-level grayscale
  config.ditherBlueNoise = blueNoise;    // blue noise vs Bayer (Display > Image Dither)
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    SdDebugLog::log("IMG", "no decoder %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    SdDebugLog::log("IMG", "decode FAILED %s decoder=%s free=%u largest=%u", imagePath.c_str(),
                    decoder->getFormatName(), (unsigned)ESP.getFreeHeap(),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    decodeFailed = true;  // don't retry on the remaining render passes for this view
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decode successful");
  SdDebugLog::log("IMG", "decode OK %s", imagePath.c_str());
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h));
}
