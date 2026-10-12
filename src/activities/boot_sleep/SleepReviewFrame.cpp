#include "SleepReviewFrame.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace SleepReviewFrame {

namespace {
constexpr char FRAME_FILE[] = "/.crosspoint/sleep_review.bin";
constexpr uint32_t MAGIC = 0x52535043;  // "CPSR"
constexpr uint8_t VERSION = 1;

// magic u32, version u8, orientation u8, reserved u16, buffer size u32, path hash u32.
constexpr size_t HEADER_BYTES = 16;

uint32_t pathHash(const std::string& path) {
  uint32_t h = 2166136261u;  // FNV-1a
  for (const char c : path) {
    h ^= static_cast<uint8_t>(c);
    h *= 16777619u;
  }
  return h;
}

void put32(uint8_t* p, const uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

void encodeHeader(const GfxRenderer& renderer, const std::string& imagePath, uint8_t out[HEADER_BYTES]) {
  memset(out, 0, HEADER_BYTES);
  put32(out, MAGIC);
  out[4] = VERSION;
  out[5] = static_cast<uint8_t>(renderer.getOrientation());
  put32(out + 8, static_cast<uint32_t>(renderer.getBufferSize()));
  put32(out + 12, pathHash(imagePath));
}
}  // namespace

void save(const GfxRenderer& renderer, const std::string& imagePath) {
  uint8_t header[HEADER_BYTES];
  encodeHeader(renderer, imagePath, header);
  const size_t size = renderer.getBufferSize();
  bool ok = false;
  {
    HalFile file;
    if (Storage.openFileForWrite("SLPR", FRAME_FILE, file)) {
      ok = file.write(header, sizeof(header)) == sizeof(header) && file.write(renderer.getFrameBuffer(), size) == size;
    }
  }
  if (!ok) {
    LOG_ERR("SLPR", "Frame save failed");
    Storage.remove(FRAME_FILE);  // a short file must not be mistaken for a frame
  }
}

bool load(GfxRenderer& renderer, const std::string& imagePath) {
  if (!Storage.exists(FRAME_FILE)) return false;
  bool ok = false;
  {
    HalFile file;
    if (Storage.openFileForRead("SLPR", FRAME_FILE, file)) {
      uint8_t header[HEADER_BYTES];
      uint8_t expected[HEADER_BYTES];
      encodeHeader(renderer, imagePath, expected);
      const size_t size = renderer.getBufferSize();
      // Header first, so a mismatch never touches the framebuffer.
      ok = file.read(header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
           memcmp(header, expected, sizeof(header)) == 0 &&
           file.read(renderer.getFrameBuffer(), size) == static_cast<int>(size);
    }
  }
  Storage.remove(FRAME_FILE);
  return ok;
}

void discard() {
  if (Storage.exists(FRAME_FILE)) Storage.remove(FRAME_FILE);
}

}  // namespace SleepReviewFrame
