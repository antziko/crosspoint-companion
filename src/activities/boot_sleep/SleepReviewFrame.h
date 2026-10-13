#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

class GfxRenderer;

// The finished frame of a random /sleep wallpaper, saved on the way into sleep so the on-wake
// review can show it without decoding the BMP again. A B/W render is one plane; a grayscale
// render adds the LSB and MSB gray planes, each captured from the framebuffer just before it
// went to the panel. The file records the wallpaper path, the orientation and the plane count;
// anything else is rejected and the review decodes as before.
namespace SleepReviewFrame {

constexpr uint8_t PLANES_BW = 1;
constexpr uint8_t PLANES_GRAY = 3;  // BW base, then gray LSB, then gray MSB

// Streams planes into the frame file as the sleep screen draws them. Every plane must be added
// before finish(); a short or failed write removes the file.
class Writer {
 public:
  bool begin(const GfxRenderer& renderer, const std::string& imagePath, uint8_t planes);
  // Appends the framebuffer as the next plane.
  void addPlane(const GfxRenderer& renderer);
  // Returns whether the complete file was written.
  bool finish();
  bool active() const { return file_.isOpen(); }

 private:
  HalFile file_;
  uint8_t expected_ = 0;
  uint8_t written_ = 0;
  bool ok_ = false;
};

// Reads a frame saved for `imagePath` in the current orientation, one plane at a time into the
// framebuffer. One-shot: the file is removed when the reader goes away, matched or not.
class Reader {
 public:
  ~Reader();
  bool open(GfxRenderer& renderer, const std::string& imagePath);
  // Fills the framebuffer with the next plane.
  bool readPlane(GfxRenderer& renderer);
  uint8_t planes() const { return planes_; }

 private:
  HalFile file_;
  uint8_t planes_ = 0;
  bool opened_ = false;
};

// Removes any saved frame.
void discard();

}  // namespace SleepReviewFrame
