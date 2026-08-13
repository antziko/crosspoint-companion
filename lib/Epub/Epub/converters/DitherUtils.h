#pragma once

#include <OrderedDither.h>  // shared blue-noise / Bayer 1-bit dither (lib/GfxRenderer)
#include <stdint.h>

// Classify an image's tonal class from its sampled pixel fractions and return the
// X4 tone curve to apply. Shared by JPEG and PNG probe paths so the classification
// logic stays in one place.
//
// Priority order:
//   white-on-dark  : dark≥50% && bright≥2%          → None     (bg must stay black)
//   grey-on-dark   : dark≥50% && bright<2%
//                    && mid∈[MIN,MAX]%               → DarkText (strong lift)
//   dark photo     : dark≥50% (else)                 → Brighten (mild lift)
//   light image    : else                            → None
inline X4Tone classifyImageTone(uint32_t darkPct, uint32_t brightPct, uint32_t midPct) {
  if (darkPct >= X4_DARK_FRACTION_PCT && brightPct >= X4_BRIGHT_FRACTION_PCT) return X4Tone::None;
  if (darkPct >= X4_DARK_FRACTION_PCT && brightPct < X4_BRIGHT_FRACTION_PCT && midPct >= X4_TEXT_FRACTION_MIN_PCT &&
      midPct <= X4_TEXT_FRACTION_MAX_PCT)
    return X4Tone::DarkText;
  if (darkPct >= X4_DARK_FRACTION_PCT) return X4Tone::Brighten;
  return X4Tone::None;
}

// Single quantization entry point used by the image converters' hot loops.
// Returns a 2-bit pixel value (0=black .. 3=white):
//   oneBit       -> 1-bit halftone, value is 0 or 3 only (X3 path). `blueNoise`
//                   selects the ordered dither field (blue noise vs Bayer).
//   useDithering -> 4-level ordered dither with tone curve selected by `tone`.
//   else         -> plain 4-level quantization (no dither, no curve).
// `tone` selects the X4 tone curve: None / Brighten (mild, default) / DarkText
// (strong lift for grey text on dark bg). Ignored on X3 and plain-quantize paths.
inline uint8_t ditherPixel(uint8_t gray, int x, int y, bool useDithering, bool oneBit, bool blueNoise,
                           X4Tone tone = X4Tone::Brighten) {
  if (oneBit) return orderedDither1Bit(gray, x, y, blueNoise) ? 3 : 0;
  if (useDithering) return orderedDither4Level(gray, x, y, blueNoise, tone);
  uint8_t q = gray / 85;
  return q > 3 ? 3 : q;
}
