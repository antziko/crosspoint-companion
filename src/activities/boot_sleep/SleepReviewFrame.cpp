#include "SleepReviewFrame.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstring>

namespace SleepReviewFrame {

namespace {
constexpr char FRAME_FILE[] = "/.crosspoint/sleep_review.bin";
constexpr uint32_t MAGIC = 0x52535043;  // "CPSR"
constexpr uint8_t VERSION = 2;

// magic u32, version u8, orientation u8, plane count u8, reserved u8, buffer size u32, path hash u32.
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

void encodeHeader(const GfxRenderer& renderer, const std::string& imagePath, const uint8_t planes,
                  uint8_t out[HEADER_BYTES]) {
  memset(out, 0, HEADER_BYTES);
  put32(out, MAGIC);
  out[4] = VERSION;
  out[5] = static_cast<uint8_t>(renderer.getOrientation());
  out[6] = planes;
  put32(out + 8, static_cast<uint32_t>(renderer.getBufferSize()));
  put32(out + 12, pathHash(imagePath));
}
}  // namespace

bool Writer::begin(const GfxRenderer& renderer, const std::string& imagePath, const uint8_t planes) {
  uint8_t header[HEADER_BYTES];
  encodeHeader(renderer, imagePath, planes, header);
  expected_ = planes;
  written_ = 0;
  ok_ = Storage.openFileForWrite("SLPR", FRAME_FILE, file_) && file_.write(header, sizeof(header)) == sizeof(header);
  return ok_;
}

void Writer::addPlane(const GfxRenderer& renderer) {
  if (!ok_) return;
  const size_t size = renderer.getBufferSize();
  ok_ = written_ < expected_ && file_.write(renderer.getFrameBuffer(), size) == size;
  written_++;
}

bool Writer::finish() {
  const bool complete = ok_ && written_ == expected_;
  file_.close();
  if (!complete) {
    LOG_ERR("SLPR", "Frame save failed (planes %u/%u)", written_, expected_);
    Storage.remove(FRAME_FILE);  // a short file must not be mistaken for a frame
  }
  ok_ = false;
  return complete;
}

Reader::~Reader() {
  if (!opened_) return;
  file_.close();  // explicit: remove follows on the same path
  Storage.remove(FRAME_FILE);
}

bool Reader::open(GfxRenderer& renderer, const std::string& imagePath) {
  if (!Storage.exists(FRAME_FILE)) return false;
  opened_ = true;
  if (!Storage.openFileForRead("SLPR", FRAME_FILE, file_)) return false;
  uint8_t header[HEADER_BYTES];
  if (file_.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  const uint8_t planes = header[6];
  if (planes != PLANES_BW && planes != PLANES_GRAY) return false;
  uint8_t expected[HEADER_BYTES];
  encodeHeader(renderer, imagePath, planes, expected);
  // Header first, so a mismatch never touches the framebuffer.
  if (memcmp(header, expected, sizeof(header)) != 0) return false;
  planes_ = planes;
  return true;
}

bool Reader::readPlane(GfxRenderer& renderer) {
  if (planes_ == 0) return false;
  const size_t size = renderer.getBufferSize();
  return file_.read(renderer.getFrameBuffer(), size) == static_cast<int>(size);
}

void discard() {
  if (Storage.exists(FRAME_FILE)) Storage.remove(FRAME_FILE);
}

}  // namespace SleepReviewFrame
