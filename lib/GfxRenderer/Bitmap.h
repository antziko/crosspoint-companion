#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "BitmapHelpers.h"
#include "OrderedDither.h"  // ImageDitherMode (IMG_DITHER_*)

#pragma pack(push, 1)
struct BmpHeader {
  struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
  } fileHeader;
  struct {
    uint32_t biSize;
    int32_t biWidth;
    int32_t biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t biXPelsPerMeter;
    int32_t biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
  } infoHeader;
  struct RgbQuad {
    uint8_t rgbBlue;
    uint8_t rgbGreen;
    uint8_t rgbRed;
    uint8_t rgbReserved;
  };
  RgbQuad colors[2];
};
#pragma pack(pop)

enum class BmpReaderError : uint8_t {
  Ok = 0,
  FileInvalid,
  SeekStartFailed,

  NotBMP,
  DIBTooSmall,

  BadPlanes,
  UnsupportedBpp,
  UnsupportedCompression,

  BadDimensions,
  ImageTooLarge,
  PaletteTooLarge,

  SeekPixelDataFailed,
  BufferTooSmall,
  OomRowBuffer,
  ShortReadRow,
};

class Bitmap {
 public:
  static const char* errorToString(BmpReaderError err);

  explicit Bitmap(HalFile& file, bool dithering = false) : file(file), dithering(dithering) {}
  ~Bitmap();

  // Render as a pure 1-bit black/white halftone (ordered dither) instead of
  // 4-level grayscale. Used on X3, whose 4-level grayscale waveform washes out;
  // a 1-bit halftone keeps full tonal detail. Call before parseHeaders().
  void setOneBitDither(bool enable) { oneBitDither = enable; }
  // Dither algorithm (Display > Image Dither, IMG_DITHER_* value):
  //   X3 (oneBitDither): blue-noise vs Bayer 1-bit field (error-diffusion maps
  //     to blue noise — X3 has no 4-level path).
  //   X4 (4-level): blue-noise/Bayer ordered, or Atkinson/FS error-diffusion.
  void setImageDitherMode(uint8_t mode) { ditherMode = mode; }
  // Tone curve for the 1-bit halftone (setOneBitDither). How much midtone lift the
  // halftone needs depends on the panel's dot gain, so the caller picks it -- see
  // kHalftoneToneX3 / kHalftoneToneX4. Defaults to the X3 curve, which is what every
  // caller that does not set it was already getting. No effect on the 4-level paths.
  void setHalftoneTone(HalftoneTone tone) { halftoneTone = tone; }
  // Quantize to the three tones the panel can actually show (black / one gray / white)
  // instead of four nominal levels, when its AA waveform drives both mid buckets the
  // same way. `grayValue` is that gray's assumed rendered luminance -- see
  // quantizeThreeLevel. 0 (the default) keeps the 4-level behaviour everywhere.
  // Call before parseHeaders(); ignored on the 1-bit halftone path.
  void setThreeLevelGray(int grayValue) { threeLevelGray = grayValue; }
  BmpReaderError parseHeaders();
  BmpReaderError readNextRow(uint8_t* data, uint8_t* rowBuffer) const;
  BmpReaderError rewindToData() const;
  int getWidth() const { return width; }
  int getHeight() const { return height; }
  bool isTopDown() const { return topDown; }
  bool hasGreyscale() const { return bpp > 1; }
  int getRowBytes() const { return rowBytes; }
  bool is1Bit() const { return bpp == 1; }
  uint16_t getBpp() const { return bpp; }

 private:
  static uint16_t readLE16(HalFile& f);
  static uint32_t readLE32(HalFile& f);

  HalFile& file;
  bool dithering = false;
  bool oneBitDither = false;                    // 1-bit halftone instead of 4-level (X3)
  uint8_t ditherMode = IMG_DITHER_BLUE_NOISE;   // IMG_DITHER_* (blue/bayer/error-diffusion)
  HalftoneTone halftoneTone = kHalftoneToneX3;  // 1-bit halftone tone curve (oneBitDither only)
  int threeLevelGray = 0;                       // >0 = 3-tone output (see setThreeLevelGray)
  bool fourLevelOrdered = false;                // X4 ordered 4-level (blue/bayer) vs error-diffusion
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t bfOffBits = 0;
  uint16_t bpp = 0;
  uint32_t colorsUsed = 0;
  bool nativePalette = false;  // true if all palette entries map to native gray levels
  int rowBytes = 0;
  uint8_t paletteLum[256] = {};

  // Dithering state (mutable for const methods)
  mutable int16_t* errorCurRow = nullptr;
  mutable int16_t* errorNextRow = nullptr;
  mutable int prevRowY = -1;  // Track row progression for error propagation

  mutable AtkinsonDitherer* atkinsonDitherer = nullptr;
  mutable FloydSteinbergDitherer* fsDitherer = nullptr;
};
