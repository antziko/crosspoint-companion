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

// X4 tone curve selector — three regimes for the 4-level ordered-dither path.
// Passed through the image-converter hot loops so each image uses the curve
// matched to its content class (detected by the pre-decode luminance probe).
//
//   None     : no tone curve; 4-level quantization with raw input luminance.
//              Used for light/white-background images and the bimodal white-text
//              path where the background must not be lifted.
//   Brighten : mild lift via toneMapX4 (gamma 0.65). Default for dark photos.
//   DarkText : stronger lift via toneMapX4DarkText (gamma 0.50 + black anchor).
//              Used for grey text on a dark background (terminal/code screenshots
//              where text is not pure white). Pushes mid-grey text up a level
//              while anchoring the background to solid black.
enum class X4Tone : uint8_t { None, Brighten, DarkText };

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
// Fill a 256-entry tone LUT. Inputs at or below `anchor` map to 0 (pure black);
// above it the remaining range is renormalized to 0..255 and gamma-lifted, so
// true blacks stay black, 255 stays white, and midtones are brightened.
// Float maths runs 256 times per table build, never per pixel (the ESP32-C3 has
// no FPU), which is why the result is cached by every caller below.
inline void buildHalftoneToneLut(uint8_t* lut, int gammaX100, int anchor) {
  const float span = 255.0f - static_cast<float>(anchor);
  const float gamma = static_cast<float>(gammaX100) / 100.0f;
  for (int i = 0; i < 256; i++) {
    if (i <= anchor || span <= 0.0f) {
      lut[i] = 0;
      continue;
    }
    const float n = (i - anchor) / span;
    const float c = (gammaX100 == 100) ? n : powf(n, gamma);
    const int v = static_cast<int>(c * 255.0f + 0.5f);
    lut[i] = v > 255 ? 255 : static_cast<uint8_t>(v);
  }
}

inline uint8_t toneMapX3(uint8_t gray) {
  static uint8_t lut[256];
  static bool ready = false;
  if (!ready) {
    buildHalftoneToneLut(lut, static_cast<int>(X3_IMAGE_GAMMA * 100.0f + 0.5f), X3_BLACK_ANCHOR);
    ready = true;
  }
  return lut[gray];
}

// --- Panel-specific halftone tone, for callers that pick their own curve -----
// The halftone mechanism is the same on every panel -- dot density fakes tone --
// but how much lift it needs is not: the correction fights the panel's dot gain,
// and a higher-contrast panel over-brightens under the X3 numbers.
//
// gammaX100 is gamma * 100 (lower = brighter midtones / more shadow detail, and
// 100 = no change); blackAnchor is the input at or below which a pixel is forced
// to pure black, so the gamma can lift midtones WITHOUT graying out true blacks.
struct HalftoneTone {
  uint8_t gammaX100;
  uint8_t blackAnchor;
};

// The X3 numbers, as constants for callers that select a tone explicitly.
inline constexpr HalftoneTone kHalftoneToneX3{70, 28};
// X4 / X4 Pro baseline. Deliberately gentler than the X3's: this panel holds a
// deeper black and spreads dots less, so the X3 lift washes its midtones out and
// its anchor throws away shadow detail the panel can actually hold.
// TUNE ON DEVICE, exactly like the X3 pair above -- Settings' Wallpaper Tone row
// shifts gammaX100 around this baseline so it can be dialled in without a build.
inline constexpr HalftoneTone kHalftoneToneX4{85, 20};

// Tone map for an explicitly chosen curve. Its own LUT, keyed on the tone, so the
// fixed X3 table above is never rebuilt and that path stays byte-identical. The
// cache assumes image rendering is single-threaded and that one image is dithered
// with one tone -- both hold here (a full-image pass runs on one task), and it is
// the same assumption toneMapX3's `ready` flag already makes.
inline uint8_t toneMapHalftone(uint8_t gray, HalftoneTone tone) {
  static uint8_t lut[256];
  static uint16_t builtKey = 0xFFFFu;
  const uint16_t key = static_cast<uint16_t>(tone.gammaX100) << 8 | tone.blackAnchor;
  if (key != builtKey) {
    buildHalftoneToneLut(lut, tone.gammaX100, tone.blackAnchor);
    builtKey = key;
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
    {0, 130, 32, 162, 8, 138, 40, 170},   {194, 65, 226, 97, 202, 73, 234, 105},
    {49, 178, 16, 146, 57, 186, 24, 154}, {243, 113, 210, 81, 251, 121, 218, 89},
    {12, 142, 44, 174, 4, 134, 36, 166},  {206, 77, 238, 109, 198, 69, 230, 101},
    {61, 190, 28, 158, 53, 182, 20, 150}, {255, 125, 222, 93, 247, 117, 214, 85},
};

// Returns true (white) / false (black). `gray` is the 8-bit luminance. The X3
// tone curve is applied first so both dither fields share the same correction.
inline bool orderedDither1Bit(uint8_t gray, int x, int y, bool blueNoise, HalftoneTone tone = kHalftoneToneX3) {
  gray = (tone.gammaX100 == kHalftoneToneX3.gammaX100 && tone.blackAnchor == kHalftoneToneX3.blackAnchor)
             ? toneMapX3(gray)
             : toneMapHalftone(gray, tone);
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

// Bimodal (high-contrast text) detection. A dark image that ALSO has a meaningful
// share of near-white pixels is almost always white text on a dark background
// (e.g. a terminal/code screenshot), not a dark photo. Brightening such an image
// lifts the black background toward grey and crushes the white-text contrast, so
// the brighten verdict skips it when both dark- and bright-fractions are high.
//   X4_BRIGHT_PIXEL_CUTOFF : luminance (0..255) at/above which a pixel is "bright".
//   X4_BRIGHT_FRACTION_PCT : treat as bimodal text when >= this % are bright.
inline constexpr uint8_t X4_BRIGHT_PIXEL_CUTOFF = 200;
// Text strokes are a SMALL share of a screenshot's pixels (thin glyphs on a large
// dark field), so this fraction is intentionally low — a couple of % of pure-white
// pixels alongside a mostly-dark image is the signature of white-on-black text.
inline constexpr uint8_t X4_BRIGHT_FRACTION_PCT = 2;

// --- Grey-text-on-dark detection (DarkText tone path) -----------------------
// A dark image whose text is GREY (not white) — typical of many terminal/code
// screenshots where text colour is e.g. #AAAAAA or similar — has no near-white
// pixels to trigger the bimodal threshold above. Without special handling those
// images fall into the "dark photo" bucket and get only the mild Brighten curve,
// leaving grey text barely one level above the black background.
//
// Detection criterion (applied only when X4_DARK_FRACTION_PCT is already met):
//   1. Near-white fraction is LOW (< X4_BRIGHT_FRACTION_PCT) — not white-on-dark.
//   2. Mid-grey fraction is in [MIN, MAX] — enough grey pixels for text but not
//      so many that it looks like a continuous-tone photo with uniform midtones.
//
//   X4_TEXT_PIXEL_CUTOFF    : lower bound of the "grey text" band.
//                             Pixels in [X4_TEXT_PIXEL_CUTOFF, X4_BRIGHT_PIXEL_CUTOFF)
//                             are counted as mid-grey candidates.
//   X4_TEXT_FRACTION_MIN_PCT: minimum % of mid-grey pixels (sparse glyph coverage).
//   X4_TEXT_FRACTION_MAX_PCT: maximum % of mid-grey pixels. A smooth-gradient photo
//                             has continuous midtones well above this; sparse text
//                             glyphs on a dark field stay under it.
//
// TUNE THESE on-device:
//   Raise TEXT_PIXEL_CUTOFF if very dark grey text isn't detected.
//   Lower TEXT_FRACTION_MAX_PCT if dark photos are wrongly classified as grey-text.
inline constexpr uint8_t X4_TEXT_PIXEL_CUTOFF = 110;
inline constexpr uint8_t X4_TEXT_FRACTION_MIN_PCT = 2;
inline constexpr uint8_t X4_TEXT_FRACTION_MAX_PCT = 25;

// --- X4 DarkText tone curve -------------------------------------------------
// Stronger lift specifically for grey-text-on-dark images. The Brighten curve
// (gamma 0.65) is too mild — grey text in the ~110–190 luminance band quantizes
// to level 1 (near-black) after the 4-level dither, invisible against the
// background. A gamma of 0.50 pushes those midtones up to levels 2–3 (visible
// grey / near-white). The black anchor (40) forces true-black background pixels
// to stay solid black so the lifted curve doesn't grey out the bg.
//
// NOTE: changing these constants changes dithered pixel data stored in the EPUB
// pixel cache. Bump the cache suffix in ImageBlock.cpp whenever retuning.
//
// TUNE THESE on-device:
//   X4_DARKTEXT_GAMMA       : lower = brighter midtones (push grey text up further).
//                             0.50 is a fairly aggressive lift; try 0.45 if still dark.
//   X4_DARKTEXT_BLACK_ANCHOR: larger = more bg pixels forced solid black. Should be
//                             just below the typical bg luminance. 40 handles most
//                             near-black backgrounds; raise to ~60 if bg greys out.
inline constexpr float X4_DARKTEXT_GAMMA = 0.50f;
inline constexpr int X4_DARKTEXT_BLACK_ANCHOR = 40;

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

// Tone LUT for the DarkText path. Mirrors toneMapX4 but uses
// X4_DARKTEXT_GAMMA / X4_DARKTEXT_BLACK_ANCHOR for a stronger lift.
inline uint8_t toneMapX4DarkText(uint8_t gray) {
  static uint8_t lut[256];
  static bool ready = false;
  if (!ready) {
    const float span = 255.0f - X4_DARKTEXT_BLACK_ANCHOR;
    for (int i = 0; i < 256; i++) {
      if (i <= X4_DARKTEXT_BLACK_ANCHOR || span <= 0.0f) {
        lut[i] = 0;
        continue;
      }
      float n = (i - X4_DARKTEXT_BLACK_ANCHOR) / span;
      float c = (X4_DARKTEXT_GAMMA == 1.0f) ? n : powf(n, X4_DARKTEXT_GAMMA);
      int v = static_cast<int>(c * 255.0f + 0.5f);
      lut[i] = v > 255 ? 255 : static_cast<uint8_t>(v);
    }
    ready = true;
  }
  return lut[gray];
}

// 4-level ordered dither (returns 0..3) with selectable X4 tone curve. Uses the
// shared 8x8 Bayer or 64x64 blue-noise field (caller picks via `blueNoise`) —
// same fields as the 1-bit path, so the "Image Dither" setting applies to X4 too,
// and blue noise replaces the visible 4x4 crosshatch. Standard ordered-dither
// quantization to 4 levels: level = floor(scaled + t), scaled = gray/255*3.
//
// `tone` selects which curve is applied before quantization:
//   None     : raw input (no lift)
//   Brighten : mild toneMapX4 (default; dark photos, BMP path)
//   DarkText : strong toneMapX4DarkText (grey text on dark background)
inline uint8_t orderedDither4Level(uint8_t gray, int x, int y, bool blueNoise, X4Tone tone = X4Tone::Brighten) {
  switch (tone) {
    case X4Tone::Brighten:
      gray = toneMapX4(gray);
      break;
    case X4Tone::DarkText:
      gray = toneMapX4DarkText(gray);
      break;
    case X4Tone::None:
      break;
  }
  const int thresh = blueNoise ? blueNoise64[y & 63][x & 63] : bayer8x8Thresh[y & 7][x & 7];
  const int level = (gray * 768 / 255 + thresh) / 256;  // 0..3
  return level > 3 ? 3 : static_cast<uint8_t>(level);
}

// --- 3-level ordered dither -------------------------------------------------
// For panels whose AA waveform drives BOTH mid-tone buckets with the same table,
// so the "4 levels" are physically three: black, one gray, white. Dithering to
// four there is actively harmful -- two of the four render identically, and the
// tone the dither placed between them is simply lost.
//
// Emits 0 / 1 / 3 only. Level 1 and level 2 are the same tone on such a panel, so
// picking one of them consistently costs nothing and keeps the output alphabet
// honest about what the hardware can show.
inline uint8_t orderedDither3Level(uint8_t gray, int x, int y, bool blueNoise, X4Tone tone = X4Tone::Brighten) {
  switch (tone) {
    case X4Tone::Brighten:
      gray = toneMapX4(gray);
      break;
    case X4Tone::DarkText:
      gray = toneMapX4DarkText(gray);
      break;
    case X4Tone::None:
      break;
  }
  const int thresh = blueNoise ? blueNoise64[y & 63][x & 63] : bayer8x8Thresh[y & 7][x & 7];
  const int level = (gray * 512 / 255 + thresh) / 256;  // 0..2
  return level >= 2 ? 3 : static_cast<uint8_t>(level);  // 0, 1, 3
}
