#include "BitmapRenderUtils.h"

#include <cmath>

namespace BitmapRenderUtils {

Placement centeredPlacement(int imgWidth, int imgHeight, int screenWidth, int screenHeight, bool crop) {
  Placement p{0, 0, 0.0f, 0.0f};
  if (imgWidth > screenWidth || imgHeight > screenHeight) {
    float ratio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
    const float screenRatio = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
    if (ratio > screenRatio) {
      // Wider than the viewport ratio: letterbox top/bottom, or trim the sides to fill.
      // Cropping brings the drawn ratio to the screen ratio, so the letterbox goes to 0.
      if (crop) {
        p.cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - p.cropX) * static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
      }
      p.x = 0;
      p.y = std::round((static_cast<float>(screenHeight) - static_cast<float>(screenWidth) / ratio) / 2);
    } else {
      // Taller than the viewport ratio: pillarbox left/right, or trim top and bottom.
      if (crop) {
        p.cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(imgWidth) / ((1.0f - p.cropY) * static_cast<float>(imgHeight));
      }
      p.x = std::round((static_cast<float>(screenWidth) - static_cast<float>(screenHeight) * ratio) / 2);
      p.y = 0;
    }
  } else {
    p.x = (screenWidth - imgWidth) / 2;
    p.y = (screenHeight - imgHeight) / 2;
  }
  return p;
}

void applyGrayscaleOverlay(GfxRenderer& renderer, const Bitmap& bitmap, int x, int y, int screenWidth, int screenHeight,
                           float cropX, float cropY) {
  bitmap.rewindToData();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderer.drawBitmap(bitmap, x, y, screenWidth, screenHeight, cropX, cropY);
  renderer.copyGrayscaleLsbBuffers();

  bitmap.rewindToData();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderer.drawBitmap(bitmap, x, y, screenWidth, screenHeight, cropX, cropY);
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

}  // namespace BitmapRenderUtils
