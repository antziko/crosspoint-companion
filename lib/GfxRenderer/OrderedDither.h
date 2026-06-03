#pragma once

#include <math.h>
#include <stdint.h>

#include "BlueNoise.h"

// Image dither algorithm (Display > Image Dither). Values match
// CrossPointSettings::IMAGE_DITHER so the raw setting can be passed straight in.
//   BlueNoise / Bayer    : stateless ordered dither (any pixel order, incl. JPEG
//                          blocks). 1-bit on X3, 4-level on X4.
//   ErrorDiffusion       : stateful Atkinson/FS — best photo quality, but needs
//                          full-row order, so only BMP/sleep (row streaming) and
//                          not JPEG block decode. EPUB falls back to BlueNoise.
enum ImageDitherMode : uint8_t {
  IMG_DITHER_BLUE_NOISE = 0,
  IMG_DITHER_BAYER = 1,
  IMG_DITHER_ERROR_DIFFUSION = 2,
};

// --- X3 tone curve (gamma) applied before 1-bit dithering -------------------
// 1-bit dithering ties white-dot density directly to the gray value, but e-ink
// "dot gain" (black dots spread larger than their cell) makes dithered midtones
// read too dark and crushes shadow detail. A gamma < 1.0 lifts midtones to
// compensate and recovers detail. It does NOT add shades — it redistributes
// dot density so the halftone matches the intended tone on the X3 panel.
//
// TUNE THESE on-device (flash -> look -> adjust):
//   X3_IMAGE_GAMMA  : 1.00 = no change. Lower = brighter midtones / more shadow
//                     detail. Lifts the whole low end.
//   X3_BLACK_ANCHOR : inputs at/below this (0..255) are forced to pure black, so
//                     gamma can lift midtones WITHOUT graying out true blacks.
//                     Larger = more pixels stay solid black. 0 = no anchor.
inline constexpr float X3_IMAGE_GAMMA = 0.70f;
inline constexpr int X3_BLACK_ANCHOR = 28;

// Precomputed 256-entry tone LUT (built once on first use; inline-function
// static is a single shared instance across translation units). After init it
// is a plain table lookup — no per-pixel float (ESP32-C3 has no FPU).
//
// Curve: inputs <= anchor -> 0 (pure black). Above the anchor, the remaining
// range is renormalized to 0..255 and gamma-lifted, so true blacks stay black,
// highlights stay white (255->255), and midtones are brightened.
inline uint8_t toneMapX3(uint8_t gray) {
  static uint8_t lut[256];
  static bool ready = false;
  if (!ready) {
    const float span = 255.0f - X3_BLACK_ANCHOR;
    for (int i = 0; i < 256; i++) {
      if (i <= X3_BLACK_ANCHOR || span <= 0.0f) {
        lut[i] = 0;
        continue;
      }
      float n = (i - X3_BLACK_ANCHOR) / span;
      float c = (X3_IMAGE_GAMMA == 1.0f) ? n : powf(n, X3_IMAGE_GAMMA);
      int v = static_cast<int>(c * 255.0f + 0.5f);
      lut[i] = v > 255 ? 255 : static_cast<uint8_t>(v);
    }
    ready = true;
  }
  return lut[gray];
}

// Stateless 1-bit ordered dither shared by every image path (EPUB converters,
// sleep wallpaper, BMP viewer). Two selectable threshold fields:
//   - blue noise (64x64 void-and-cluster, BlueNoise.h): organic dot scatter,
//     no visible grid — best for photographic content.
//   - Bayer 8x8: classic ordered dither, regular crosshatch texture.
// Both are stateless lookups, so they work with any pixel order (row streaming
// or JPEG MCU blocks). Caller picks via `blueNoise`.

// 8x8 Bayer matrix pre-scaled to the 0..255 gray domain ((v * 255) / 63).
inline const uint8_t bayer8x8Thresh[8][8] = {
    {0, 130, 32, 162, 8, 138, 40, 170},     {194, 65, 226, 97, 202, 73, 234, 105},
    {49, 178, 16, 146, 57, 186, 24, 154},   {243, 113, 210, 81, 251, 121, 218, 89},
    {12, 142, 44, 174, 4, 134, 36, 166},    {206, 77, 238, 109, 198, 69, 230, 101},
    {61, 190, 28, 158, 53, 182, 20, 150},   {255, 125, 222, 93, 247, 117, 214, 85},
};

// Returns true (white) / false (black). `gray` is the 8-bit luminance. The X3
// tone curve is applied first so both dither fields share the same correction.
inline bool orderedDither1Bit(uint8_t gray, int x, int y, bool blueNoise) {
  gray = toneMapX3(gray);
  int t = blueNoise ? blueNoise64[y & 63][x & 63] : bayer8x8Thresh[y & 7][x & 7];
  // The threshold fields span the full 0..255 range, so a cell holding 255 would
  // keep a black dot even on pure white (gray > 255 is never true) — a dotted
  // gray tint, most visible with the regular Bayer grid. Compress to [2,251] so
  // pure white (255) always beats the threshold and pure black (0) never does.
  t = (t * 251) / 256 + 2;
  return gray > t;
}

// --- X4 tone curve for the 4-level (native grayscale) paths -----------------
// X4 (SSD1677) renders true 4-level grays, so it needs only MILD dot-gain
// correction (the X3 curve is aggressive because a 1-bit halftone fakes tone
// purely with dot density). e-ink still spreads black, so midtones read a bit
// dark; a gentle gamma lifts them and a small anchor keeps deep shadows solid.
// With only 4 levels, dark-grey detail (e.g. grey terminal text on a dark
// background) collapses into level 0/1 and reads as solid black. A gentle gamma
// < 1.0 lifts shadows/midtones up a level so that detail separates from true
// black, while white (255) stays white and black (0) stays black (no anchor).
// 0.75 is a mild lift; lower it further (toward ~0.6) if greys still read too
// dark, or back toward 1.0 if light tones start blowing out to white.
// NOTE: changing this changes the dithered pixel data — the EPUB pixel cache is
// suffix-versioned (ImageBlock.cpp), so bump that suffix when retuning so stale
// (darker) caches regenerate. BMP/sleep decode live, so they update for free.
inline constexpr float X4_IMAGE_GAMMA = 0.65f;
inline constexpr int X4_BLACK_ANCHOR = 0;

// Dark-background detection for the conditional X4 brighten curve. An image is
// brightened only when a large SHARE of its pixels are dark — not when the mean
// is low. Mean is fooled by bimodal images (e.g. a white-background terminal shot
// with a dark title bar averages below mid-grey yet is clearly light); counting
// dark pixels instead keeps white backgrounds from washing out.
//   X4_DARK_PIXEL_CUTOFF : luminance (0..255) at/below which a pixel is "dark".
//   X4_DARK_FRACTION_PCT : brighten only if >= this % of sampled pixels are dark.
// Only the EPUB image converters (JPEG/PNG) consult these; BMP/sleep always lift.
inline constexpr uint8_t X4_DARK_PIXEL_CUTOFF = 80;
inline constexpr uint8_t X4_DARK_FRACTION_PCT = 50;

inline uint8_t toneMapX4(uint8_t gray) {
  static uint8_t lut[256];
  static bool ready = false;
  if (!ready) {
    const float span = 255.0f - X4_BLACK_ANCHOR;
    for (int i = 0; i < 256; i++) {
      if (i <= X4_BLACK_ANCHOR || span <= 0.0f) {
        lut[i] = 0;
        continue;
      }
      float n = (i - X4_BLACK_ANCHOR) / span;
      float c = (X4_IMAGE_GAMMA == 1.0f) ? n : powf(n, X4_IMAGE_GAMMA);
      int v = static_cast<int>(c * 255.0f + 0.5f);
      lut[i] = v > 255 ? 255 : static_cast<uint8_t>(v);
    }
    ready = true;
  }
  return lut[gray];
}

// 4-level ordered dither (returns 0..3) with the X4 tone curve. Uses the shared
// 8x8 Bayer or 64x64 blue-noise field (caller picks via `blueNoise`) — same
// fields as the 1-bit path, so the "Image Dither" setting applies to X4 too,
// and blue noise replaces the visible 4x4 crosshatch. Standard ordered-dither
// quantization to 4 levels: level = floor(scaled + t), scaled = gray/255*3.
inline uint8_t orderedDither4Level(uint8_t gray, int x, int y, bool blueNoise, bool brighten = true) {
  if (brighten) gray = toneMapX4(gray);
  const int thresh = blueNoise ? blueNoise64[y & 63][x & 63] : bayer8x8Thresh[y & 7][x & 7];
  const int level = (gray * 768 / 255 + thresh) / 256;  // 0..3
  return level > 3 ? 3 : static_cast<uint8_t>(level);
}
