#pragma once

#include <stdint.h>

#include <OrderedDither.h>  // shared blue-noise / Bayer 1-bit dither (lib/GfxRenderer)

// 4x4 Bayer matrix for ordered dithering
inline const uint8_t bayer4x4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Apply Bayer dithering and quantize to 4 levels (0-3)
// Stateless - works correctly with any pixel processing order
inline uint8_t applyBayerDither4Level(uint8_t gray, int x, int y) {
  int bayer = bayer4x4[y & 3][x & 3];
  int dither = (bayer - 8) * 5;  // Scale to +/-40 (half of quantization step 85)

  int adjusted = gray + dither;
  if (adjusted < 0) adjusted = 0;
  if (adjusted > 255) adjusted = 255;

  if (adjusted < 64) return 0;
  if (adjusted < 128) return 1;
  if (adjusted < 192) return 2;
  return 3;
}

// Single quantization entry point used by the image converters' hot loops.
// Returns a 2-bit pixel value (0=black .. 3=white):
//   oneBit       -> 1-bit halftone, value is 0 or 3 only (X3 path). `blueNoise`
//                   selects the ordered dither field (blue noise vs Bayer).
//   useDithering -> 4-level Bayer dither.
//   else         -> plain 4-level quantization.
// `brighten` gates the X4 tone curve in the 4-level path: callers pass the
// per-image dark-background verdict so light/white-bg images aren't washed out.
// Ignored on the 1-bit (X3) and plain-quantize branches.
inline uint8_t ditherPixel(uint8_t gray, int x, int y, bool useDithering, bool oneBit, bool blueNoise,
                           bool brighten = true) {
  if (oneBit) return orderedDither1Bit(gray, x, y, blueNoise) ? 3 : 0;
  // X4: 4-level ordered dither with the X4 tone curve, blue-noise or 8x8 Bayer
  // per the "Image Dither" setting (replaces the flat 4x4 applyBayerDither4Level).
  if (useDithering) return orderedDither4Level(gray, x, y, blueNoise, brighten);
  uint8_t q = gray / 85;
  return q > 3 ? 3 : q;
}
