#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering.
  // crop=true fills the target box and lets the image overflow it on one axis (the whole
  // image is still written, just larger than the box). crop=false fits the image INSIDE the
  // box, so a tile sized to the box can draw it whole with neither a crop nor a downscale --
  // and downscaling a 1-bit dithered bitmap is what darkens it (see GfxRenderer::drawBitmap).
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              bool crop = true);
};
