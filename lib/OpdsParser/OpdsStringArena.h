#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Packed, chunk-backed storage for the short strings an OPDS feed produces.
 *
 * Why this exists: the OPDS browser's failure mode is contiguous-block starvation, not
 * free-heap exhaustion. A feed held its text as three `std::string`s per entry, so a
 * 48-entry page pinned ~144 separate small blocks scattered through the heap and made
 * `sizeof(OpdsEntry)` 76 bytes, which in turn made the entry vector's growth step the
 * largest single allocation of a parse. A device capture shows exactly that killing a
 * feed mid-parse: `entries growth bailed: count=48 need=6304 largest=5876` — 428 bytes
 * short of a 56 x 76 reallocation, with 12196 bytes still free.
 *
 * Here the text is copied into a chain of fixed 512-byte chunks. Every allocation this
 * makes is 512 bytes, a size the starved heap can always satisfy, and it stops growing
 * with the feed. Entries then hold bare `const char*` into a chunk instead of a string,
 * taking the struct to 16 bytes.
 *
 * Pointers are stable for the arena's lifetime: chunk payloads are individually owned
 * (`unique_ptr<char[]>`), so growing the chunk vector moves the handles, never the
 * characters. Strings never straddle a chunk boundary and are stored NUL-terminated, so
 * the returned pointer is a valid C string and can go straight to `drawText`/`snprintf`
 * — the `string_view` hazard in CLAUDE.md does not apply.
 *
 * Not a general allocator: there is no free-one, only `clear()`. A feed is loaded and
 * dropped whole, which is what makes that acceptable.
 */
class OpdsStringArena final {
 public:
  OpdsStringArena() = default;
  OpdsStringArena(OpdsStringArena&&) noexcept = default;
  OpdsStringArena& operator=(OpdsStringArena&&) noexcept = default;
  OpdsStringArena(const OpdsStringArena&) = delete;
  OpdsStringArena& operator=(const OpdsStringArena&) = delete;

  /**
   * Copy `len` bytes of `text` into the arena.
   * @return a stable NUL-terminated pointer, or nullptr when no chunk could be bought
   *         (out of memory, or the chunk ceiling was hit). An empty string costs no
   *         arena space and returns a static "".
   */
  const char* add(const char* text, size_t len);
  const char* add(const std::string& text) { return add(text.c_str(), text.size()); }

  /** Release every chunk. All pointers previously handed out become dangling. */
  void clear();

  size_t chunkCount() const { return chunks.size(); }
  /** Bytes of text actually stored, NUL terminators included. */
  size_t bytesUsed() const;

  static constexpr size_t CHUNK_BYTES = 512;

 private:
  // 64 x 512B = 32KB, far above what MAX_ENTRIES worth of titles can reach. A backstop
  // against a malformed feed, not a working limit.
  static constexpr size_t MAX_CHUNKS = 64;

  struct Chunk {
    std::unique_ptr<char[]> data;
    uint16_t size = 0;
    uint16_t used = 0;
  };

  std::vector<Chunk> chunks;
};
