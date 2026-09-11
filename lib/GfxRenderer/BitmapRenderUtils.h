#pragma once
#include "Bitmap.h"
#include "GfxRenderer.h"

// Shared full-screen bitmap rendering helpers. Extracted from the duplicated copies in
// SleepActivity, BmpViewerActivity and SleepImageReviewActivity so the placement maths
// and the 4-level grayscale overlay sequence live in one place.
namespace BitmapRenderUtils {

struct Placement {
  int x;
  int y;
  // Fraction of the source image trimmed off each axis before drawing, for
  // GfxRenderer::drawBitmap(). Non-zero only when `crop` was requested and the image
  // overflows the viewport; exactly one of the two can be non-zero.
  float cropX = 0.0f;
  float cropY = 0.0f;
};

// Centre an image on screen: scale-to-fit (letterboxed) when larger than the viewport,
// else centred at native size. With `crop`, an oversized image instead fills the viewport
// and the overflowing axis is trimmed (Sleep Screen Cover Mode = Crop). An image smaller
// than the viewport is never upscaled or cropped, whatever `crop` says.
Placement centeredPlacement(int imgWidth, int imgHeight, int screenWidth, int screenHeight, bool crop = false);

// Render the 4-level grayscale overlay (LSB then MSB planes) for an already-placed bitmap
// and drive the panel with the combined gray frame. The caller is responsible for the
// prior BW pass; call this only when bitmap.hasGreyscale() on a grayscale-capable panel.
// Leaves the renderer back in BW mode. cropX/cropY mirror GfxRenderer::drawBitmap().
void applyGrayscaleOverlay(GfxRenderer& renderer, const Bitmap& bitmap, int x, int y, int screenWidth, int screenHeight,
                           float cropX = 0.0f, float cropY = 0.0f);

}  // namespace BitmapRenderUtils
