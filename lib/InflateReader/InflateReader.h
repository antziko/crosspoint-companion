#pragma once

#include <uzlib.h>

#include <cstddef>

// Return value for readAtMost().
enum class InflateStatus {
  Ok,     // Output buffer full; more compressed data remains.
  Done,   // Stream ended cleanly (TINF_DONE). produced may be < maxLen.
  Error,  // Decompression failed.
};

// Streaming deflate decompressor wrapping uzlib.
//
// NOTE: retained ONLY for FontDecompressor's tiny one-shot flash-resident group
// decompressions, where uzlib's ~1KB state beats tinfl's ~11KB on the
// OOM-sensitive render path. All throughput paths (zip entries, PNG IDAT) use
// InflateStream (lib/miniz), which decodes several times faster.
//
// Two modes:
//   init(false)  — one-shot: input is a contiguous buffer, call read() once.
//   init(true)   — streaming: allocates a 32KB ring buffer for back-references
//                  across multiple read() / readAtMost() calls.
//
// Streaming callback pattern:
//   The uzlib read callback receives a `struct uzlib_uncomp*` with no separate
//   context pointer. To attach context, make InflateReader the *first member* of
//   your context struct, then cast inside the callback:
//
//     struct MyCtx {
//       InflateReader reader;   // must be first
//       HalFile* file;
//       // ...
//     };
//     static int myCb(struct uzlib_uncomp* u) {
//       MyCtx* ctx = reinterpret_cast<MyCtx*>(u);   // valid: reader.decomp is at offset 0
//       // ... fill u->source / u->source_limit, return first byte
//     }
//     MyCtx ctx;
//     ctx.reader.init(true);
//     ctx.reader.setReadCallback(myCb);
//
class InflateReader {
 public:
  InflateReader() = default;
  ~InflateReader();

  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialise decompressor. streaming=true allocates a 32KB ring buffer needed
  // when read() or readAtMost() will be called multiple times.
  // Returns false only in streaming mode if the ring buffer allocation fails.
  bool init(bool streaming = false);

  // Release the ring buffer and reset internal state.
  void deinit();

  // Set the entire compressed input as a contiguous memory buffer.
  // Used in one-shot mode; not needed when a read callback is set.
  void setSource(const uint8_t* src, size_t len);

  // Set a uzlib-compatible read callback for streaming input.
  // See class-level comment for the expected callback/context struct pattern.
  void setReadCallback(int (*cb)(uzlib_uncomp*));

  // Consume the 2-byte zlib header (CMF + FLG) from the input stream.
  // Call this once before the first read() when input is zlib-wrapped (e.g. PNG IDAT).
  void skipZlibHeader();

  // Parse and consume the gzip header from the input stream via uzlib_gzip_parse_header().
  // Call this once before read()/readAtMost() when input is a .dict.dz gzip file.
  // Returns true if the header is valid, false if the stream is not a valid gzip file.
  bool skipGzipHeader();

  // Decompress exactly len bytes into dest.
  // Returns false if the stream ends before producing len bytes, or on error.
  bool read(uint8_t* dest, size_t len);

  // Decompress up to maxLen bytes into dest.
  // Sets *produced to the number of bytes written.
  // Returns Done when the stream ends cleanly, Ok when there is more to read,
  // and Error on failure.
  InflateStatus readAtMost(uint8_t* dest, size_t maxLen, size_t* produced);

  // Returns a pointer to the underlying TINF_DATA.
  // Useful for advanced streaming setups where the callback needs access to the
  // uzlib struct directly (e.g. updating source/source_limit).
  uzlib_uncomp* raw() { return &decomp; }

  // Borrow the reserved 32KB DEFLATE window as a general scratch buffer. Lets a
  // caller that needs a large transient buffer (<= 32768 bytes) avoid malloc when
  // the heap is too fragmented to satisfy it. Returns nullptr if the window is in
  // use (an inflate is active, or another borrow is outstanding) or `need` is too
  // large; the caller must then fall back. The buffer is 8-byte aligned.
  // Pair every successful acquireScratch() with exactly one releaseScratch().
  static uint8_t* acquireScratch(size_t need);
  static void releaseScratch();

  // Allocate the shared 32KB DEFLATE window. Call once at boot (setup()) while the
  // heap is pristine so the block is guaranteed contiguous — the same guarantee the
  // old static-BSS array gave, but now freeable. Idempotent. Returns false if the
  // allocation fails, in which case init(true)/acquireScratch fall back to per-call
  // malloc (the pre-reservation behaviour).
  static bool ensureWindow();

  // Free the shared window to hand its 32KB back to the heap (e.g. to make room for
  // a TLS handshake). Only frees when the window is not currently in use; a safe
  // no-op otherwise or if already freed. After release, init(true)/acquireScratch
  // fall back to malloc until ensureWindow() runs again (typically the next boot).
  static void releaseWindow();

 private:
  uzlib_uncomp decomp = {};  // MUST stay first (offset 0) for the uzlib callback cast
  uint8_t* ringBuffer = nullptr;
  bool usingStaticWindow = false;  // true => ringBuffer points at the shared static window, not malloc
};
