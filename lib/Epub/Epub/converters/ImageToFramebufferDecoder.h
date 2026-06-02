#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  // When true, dither to a pure 1-bit black/white halftone (pixel values 0 or 3
  // only) instead of 4 gray levels. Used on X3, whose 4-level grayscale waveform
  // is weak; 1-bit values also contribute nothing to grayscale planes, so the
  // halftone survives a text-AA pass untouched.
  bool oneBitDither = false;
  // When oneBitDither is set, choose the ordered dither field: true = blue
  // noise (organic, photographic), false = Bayer (regular grid). Driven by the
  // user's Display > Image Dither setting.
  bool ditherBlueNoise = true;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

 protected:
  // Size validation helpers
  static constexpr int MAX_SOURCE_PIXELS = 3145728;  // 2048 * 1536

  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
