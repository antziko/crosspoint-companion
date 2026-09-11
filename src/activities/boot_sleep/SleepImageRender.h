#pragma once

#include <Bitmap.h>
#include <GfxRenderer.h>

#include <algorithm>

#include "CrossPointSettings.h"

// The wallpaper pipeline's one branch, shared by the sleep render and the on-wake review
// so both draw the SAME image for the same file.
//
// It decides two things that MUST agree, because Bitmap dithers for a target depth that
// only this answer defines:
//   * true  -> 4-level dither (0=black, 1=dark gray, 2=light gray, 3=white), B/W base plus
//              the AA gray planes. All four levels reach the panel.
//   * false -> 1-bit halftone. The B/W pass alone is drawn, and it maps `val < 3` to black,
//              so a 4-level dither here would crush black, dark gray AND light gray into
//              solid black and keep only pure white.
//
// The cover filter is part of the answer, not just the board: any filter other than None
// suppresses the gray planes, which leaves the B/W pass as the whole render.
inline bool sleepImageUsesGrayscale(const GfxRenderer& renderer) {
  return !renderer.isX3() &&
         SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
}

// Fill the screen and trim the overflowing axis (Sleep Screen Cover Mode = Crop)
// instead of letterboxing the whole image.
inline bool sleepImageCrops() {
  return SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
}

// Invert the painted framebuffer (Sleep Screen Cover Filter = Inverted B&W). Only ever
// true alongside a 1-bit render: sleepImageUsesGrayscale() is false for every filter, so
// this never has to invert the gray planes.
inline bool sleepImageInverts() {
  return SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE;
}

// Dither target + tone for a wallpaper bitmap. Call before parseHeaders(), which is
// why it takes none of the header-derived state: the decision above needs only the
// board and the filter, both known upfront.
inline void configureSleepBitmap(Bitmap& bitmap, const GfxRenderer& renderer) {
  const bool grayscale = sleepImageUsesGrayscale(renderer);
  bitmap.setOneBitDither(!grayscale);
  bitmap.setImageDitherMode(SETTINGS.imageDither);

  // Wallpaper Tone means the same thing on both paths -- how bright the midtones sit --
  // but the two express it differently: the halftone shifts its gamma, the 3-tone path
  // shifts the luminance it credits each gray dot with. Higher credit = fewer dots = darker.
  const uint8_t step = SETTINGS.wallpaperTone < CrossPointSettings::WALLPAPER_TONE_COUNT
                           ? SETTINGS.wallpaperTone
                           : CrossPointSettings::WALLPAPER_TONE_NORMAL;

  if (grayscale) {
    // This panel's AA waveform drives both mid buckets identically, so it shows three
    // tones, not four. Quantize to three: dithering to four spends tone on a distinction
    // the hardware cannot render, which lifts every shadow onto the single gray.
    // kAssumedGray is NOT measured -- no reference for it exists -- so it is the knob
    // Wallpaper Tone moves.
    constexpr int kAssumedGray = 95;
    constexpr int kGrayStep = 12;
    const int gray = kAssumedGray + CrossPointSettings::WALLPAPER_TONE_GAMMA_OFFSET[step] * kGrayStep / 8;
    bitmap.setThreeLevelGray(std::clamp(gray, 40, 180));
    return;
  }

  // Panel baseline, shifted by the user's Wallpaper Tone step. Applying the step as an
  // OFFSET keeps one setting meaning one thing on every board: Normal is always "this
  // panel's own curve", so the row needs no board-conditional bound.
  HalftoneTone tone = renderer.isX3() ? kHalftoneToneX3 : kHalftoneToneX4;
  const int gamma = tone.gammaX100 + CrossPointSettings::WALLPAPER_TONE_GAMMA_OFFSET[step];
  // 30..100: below ~0.30 the lift is degenerate, and 100 is "no correction at all".
  tone.gammaX100 = static_cast<uint8_t>(std::clamp(gamma, 30, 100));
  bitmap.setHalftoneTone(tone);
}
