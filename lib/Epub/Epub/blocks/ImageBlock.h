#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  // The image's number within its chapter: the n of the cache name ".../img_<spine>_<n>.<ext>",
  // assigned in document order by the parser. UINT16_MAX when the name carries none.
  uint16_t chapterImageIndex() const;
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  // Cache validity is render-mode specific: the cache filename encodes oneBit/blueNoise
  // (dither variant), so these take the renderer to check the exact file the next render
  // would use. Without it, needsDecode() could miss a mode-specific cache and always
  // draw a placeholder, or skip one that won't match.
  bool hasValidCache(const GfxRenderer& renderer) const;
  bool needsDecode(const GfxRenderer& renderer) const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearSessionRenderFailures();
  // True once after a render where an image's extraction or decode first failed for lack of heap:
  // the reader frees memory and redraws the page. A second failure of the same image is
  // remembered as usual.
  static bool takeHeapRetry();
  // Whether takeHeapRetry() would return true, without consuming it.
  static bool heapRetryQueued();

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  static void releaseRenderCache();
  // Scale w x h down (never up) to fit inside availW x availH, preserving aspect ratio and
  // clamping to >= 1. Used by the vertical reader's image-page fit.
  static void fitWithin(int availW, int availH, int& w, int& h);
  // Whether an image shown at w x h fills the page: it reaches (nearly) the full width or height.
  static bool fillsPage(int w, int h, int pageW, int pageH) {
    return w > 0 && h > 0 && (w * 10 >= pageW * 9 || h * 10 >= pageH * 9);
  }

  // Lazy extraction hook: the section build only header-probes images for their
  // dimensions; the file at imagePath is extracted out of the book on first
  // render, via this callback (function pointer + context, not std::function —
  // this is render-loop code). Registered by the reader activity that owns the
  // Epub, cleared on its exit.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;  // book-internal source href; empty once known-extracted
  int16_t width;
  int16_t height;
  // Set once a decode fails (e.g. source exceeds the decoder's max-pixel limit, or
  // OOM). The page is re-rendered many times per display (BW + grayscale strips);
  // without this, an un-decodable image is re-decoded — and re-fails — on every
  // pass, hanging the page for tens of seconds and churning the heap. Persists for
  // the life of this block (the current section view), so we retry at most once.
  bool decodeFailed = false;

  static void* extractCtx;
  static ExtractFn extractFn;
};
