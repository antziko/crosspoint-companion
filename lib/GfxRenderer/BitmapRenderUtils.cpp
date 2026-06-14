#include "BitmapRenderUtils.h"

#include <cmath>

namespace BitmapRenderUtils {

Placement centeredPlacement(int imgWidth, int imgHeight, int screenWidth, int screenHeight) {
  Placement p{0, 0};
  if (imgWidth > screenWidth || imgHeight > screenHeight) {
    const float ratio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
    const float screenRatio = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
    if (ratio > screenRatio) {
      // Wider than the viewport ratio: letterbox top/bottom.
      p.x = 0;
      p.y = std::round((static_cast<float>(screenHeight) - static_cast<float>(screenWidth) / ratio) / 2);
    } else {
      // Taller than the viewport ratio: pillarbox left/right.
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
