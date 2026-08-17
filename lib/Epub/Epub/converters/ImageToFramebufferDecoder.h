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

  // Call from per-row/per-MCU decode callbacks (free functions, hence public):
  // yields one tick at most every 250 ms so a multi-second decode keeps the idle
  // task -- and its watchdog -- fed. `lastYieldMs` is caller-held state,
  // initialised to the decode start time.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions BEFORE narrowing them into
  // ImageDimensions' int16_t fields. Without this the narrowing is silent: a
  // 40000-pixel-wide PNG stored straight into int16_t comes back negative.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format);

 protected:
  // Source-image cap. Bounds decode TIME, not memory: both decoders stream (JPEG
  // in MCU bands at 1/2..1/8 coarse scale, PNG scanline-by-scanline with its own
  // width-based row-buffer guard), so RAM never scales with source area. 8 MP
  // admits real-world ebook covers (KDP recommends 1600x2560 and 2000x3000)
  // which the old 3 MP cap rejected outright; the row callbacks yield
  // periodically so the longer decode cannot starve the watchdog.
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;  // 8 MP (e.g. 2048 * 4096)

  // Cap on the grid a decoder actually walks, after JPEGDEC's built-in 1/2..1/8
  // downscale. Deliberately NOT raised with MAX_SOURCE_PIXELS: that one is about
  // what we accept, this one is about what we are willing to spend. See
  // JpegToFramebufferConverter::decodeToFramebuffer.
  static constexpr int MAX_DECODE_GRID_PIXELS = 3145728;  // 2048 * 1536

  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
