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
};

// Centre an image on screen: scale-to-fit (letterboxed) when larger than the viewport,
// else centred at native size. Matches the placement used by the BMP viewer and the
// on-wake sleep-image review (no crop). SleepActivity keeps its own crop-aware variant.
Placement centeredPlacement(int imgWidth, int imgHeight, int screenWidth, int screenHeight);

// Render the 4-level grayscale overlay (LSB then MSB planes) for an already-placed bitmap
// and drive the panel with the combined gray frame. The caller is responsible for the
// prior BW pass; call this only when bitmap.hasGreyscale() on a grayscale-capable panel.
// Leaves the renderer back in BW mode. cropX/cropY mirror GfxRenderer::drawBitmap().
void applyGrayscaleOverlay(GfxRenderer& renderer, const Bitmap& bitmap, int x, int y, int screenWidth, int screenHeight,
                           float cropX = 0.0f, float cropY = 0.0f);

}  // namespace BitmapRenderUtils
