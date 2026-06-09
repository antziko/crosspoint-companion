#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
  // Set once a decode fails (e.g. source exceeds the decoder's max-pixel limit, or
  // OOM). The page is re-rendered many times per display (BW + grayscale strips);
  // without this, an un-decodable image is re-decoded — and re-fails — on every
  // pass, hanging the page for tens of seconds and churning the heap. Persists for
  // the life of this block (the current section view), so we retry at most once.
  bool decodeFailed = false;
};
