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

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  static void releaseRenderCache();

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
