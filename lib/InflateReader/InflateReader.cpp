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
// fragmenter. Reserve it once (at boot, via ensureWindow(), while the heap is
// pristine so the block is contiguous) and hand it out via an in-use flag: callers
// get a guaranteed contiguous window with zero per-build malloc churn. The rare
// concurrent inflate (e.g. web-server task while reading) falls back to malloc —
// never worse than before.
//
// It lives on the heap (not BSS) specifically so it can be released for the
// duration of a TLS handshake (see releaseWindow()); the mandatory post-sync
// reboot re-runs ensureWindow() on a fresh heap, so it is never re-allocated under
// fragmentation. malloc on ESP32 returns an 8-byte-aligned block, so the buffer
// can be safely borrowed as a wider type (uint32_t[]) on RISC-V.
uint8_t* s_inflateWindow = nullptr;
std::atomic_flag s_inflateWindowInUse;
}  // namespace

// Guarantee the cast pattern in the header comment is valid.
static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must be standard-layout for the uzlib callback cast to work");

uint8_t* InflateReader::acquireScratch(size_t need) {
  if (need > INFLATE_DICT_SIZE) return nullptr;
  if (!s_inflateWindow) return nullptr;                                                // released — caller mallocs
  if (s_inflateWindowInUse.test_and_set(std::memory_order_acquire)) return nullptr;    // window busy
  return s_inflateWindow;
}

void InflateReader::releaseScratch() { s_inflateWindowInUse.clear(std::memory_order_release); }

bool InflateReader::ensureWindow() {
  if (s_inflateWindow) return true;
  // Pristine-heap boot allocation: guaranteed contiguous, no churn afterwards.
  s_inflateWindow = static_cast<uint8_t*>(malloc(INFLATE_DICT_SIZE));
  return s_inflateWindow != nullptr;
}

void InflateReader::releaseWindow() {
  // Refuse while a borrower holds it (an inflate or scratch is active): taking the
  // flag both proves it's free and blocks a concurrent acquire during the free.
  if (s_inflateWindowInUse.test_and_set(std::memory_order_acquire)) return;  // busy — leave it
  free(s_inflateWindow);
  s_inflateWindow = nullptr;
  s_inflateWindowInUse.clear(std::memory_order_release);
}

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming) {
  deinit();  // release any previously held window and reset state

  if (streaming) {
    // Prefer the shared reserved window: guaranteed 32KB contiguous regardless of
    // heap fragmentation, and no malloc/free churn. Fall back to malloc if it is
    // null (released for a TLS handshake) or already held by another inflate (rare
    // — inflate is sequential on the UI task).
    if (s_inflateWindow && !s_inflateWindowInUse.test_and_set(std::memory_order_acquire)) {
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
