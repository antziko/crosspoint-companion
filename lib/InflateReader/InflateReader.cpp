#include "InflateReader.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {
constexpr size_t INFLATE_DICT_SIZE = 32768;

// DEFLATE's 32KB back-reference window can't be malloc'd reliably once the heap
// fragments during a reading session (largest free block dips below 32KB), which
// broke every uncached section build ("out of bounds") and cover-thumb decode.
// Worse, malloc-ing+freeing this 32KB on every section build was itself a primary
// fragmenter. Reserve it once in BSS and hand it out via an in-use flag: callers
// get a guaranteed contiguous window with zero malloc churn. The rare concurrent
// inflate (e.g. web-server task while reading) falls back to malloc — never worse
// than before. Zero-initialised static => the flag starts clear.
uint8_t s_inflateWindow[INFLATE_DICT_SIZE];
std::atomic_flag s_inflateWindowInUse;
}  // namespace

// Guarantee the cast pattern in the header comment is valid.
static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must be standard-layout for the uzlib callback cast to work");

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming) {
  deinit();  // release any previously held window and reset state

  if (streaming) {
    // Prefer the shared static window: guaranteed 32KB contiguous regardless of
    // heap fragmentation, and no malloc/free churn. If another inflate already
    // holds it (rare — inflate is sequential on the UI task), fall back to malloc.
    if (!s_inflateWindowInUse.test_and_set(std::memory_order_acquire)) {
      ringBuffer = s_inflateWindow;
      usingStaticWindow = true;
    } else {
      ringBuffer = static_cast<uint8_t*>(malloc(INFLATE_DICT_SIZE));
      if (!ringBuffer) return false;
    }
    memset(ringBuffer, 0, INFLATE_DICT_SIZE);
  }

  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? INFLATE_DICT_SIZE : 0);
  return true;
}

void InflateReader::deinit() {
  if (usingStaticWindow) {
    s_inflateWindowInUse.clear(std::memory_order_release);
    usingStaticWindow = false;
    ringBuffer = nullptr;
  } else if (ringBuffer) {
    free(ringBuffer);
    ringBuffer = nullptr;
  }
  memset(&decomp, 0, sizeof(decomp));
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

void InflateReader::setReadCallback(int (*cb)(struct uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::skipZlibHeader() {
  uzlib_get_byte(&decomp);
  uzlib_get_byte(&decomp);
}

bool InflateReader::skipGzipHeader() { return uzlib_gzip_parse_header(&decomp) == TINF_OK; }

bool InflateReader::read(uint8_t* dest, size_t len) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when read() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  if (!ringBuffer) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when readAtMost() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);

  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}
