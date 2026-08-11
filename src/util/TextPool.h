#pragma once

#include <cstdint>
#include <new>
#include <string>

// Shared string-pool primitive. Stores many short strings contiguously in one
// std::string buffer; callers keep {offset, length} references instead of owning
// a std::string each — collapsing N small heap allocations into ~1 growing
// buffer (less fragmentation on the constrained ESP32-C3 heap).
//
// Each appended string is null-terminated, so `pool.data() + offset` is a valid
// C string for any C API (drawText, snprintf, ...). The pool itself may contain
// embedded nulls (one per entry); index entries by their stored offset.
namespace TextPool {

// Append a copy of s[0..len) plus a '\0' to `pool`; return the byte offset of the
// copy. Manual +256 linear growth avoids std::string's 2x doubling (which would
// over-reserve and re-fragment on a small heap).
//
// NOTE: this overload aborts on OOM (a failed std::string growth calls
// std::__throw_bad_alloc() -> abort() under -fno-exceptions) and silently truncates the
// returned offset once the pool passes 64KB. Prefer appendNoThrow() below in any path that
// can run on a stressed heap.
inline uint16_t append(std::string& pool, const char* s, size_t len) {
  const uint16_t offset = static_cast<uint16_t>(pool.size());
  if (pool.size() + len + 1 > pool.capacity()) pool.reserve(pool.capacity() + 256);
  pool.append(s, len);
  pool.push_back('\0');
  return offset;
}

// As append(), but reports failure instead of aborting: probes the growth with
// operator new(nothrow) before letting std::string take it, and rejects an append that
// would push the pool past the uint16_t offset range callers store. On false, `pool` is
// unchanged and `outOffset` untouched — the caller drops the entry rather than recording a
// truncated or partially-written one.
[[nodiscard]] inline bool appendNoThrow(std::string& pool, const char* s, size_t len, uint16_t& outOffset) {
  const size_t needed = pool.size() + len + 1;
  if (needed > UINT16_MAX) return false;  // offset/len are uint16_t; refuse to wrap silently
  if (needed > pool.capacity()) {
    const size_t linear = pool.capacity() + 256;
    const size_t want = linear > needed ? linear : needed;
    // +1 covers the null std::string keeps beyond size() in its own buffer.
    void* probe = ::operator new(want + 1, std::nothrow);
    if (!probe) return false;
    ::operator delete(probe, std::nothrow);
    pool.reserve(want);
  }
  outOffset = static_cast<uint16_t>(pool.size());
  pool.append(s, len);  // capacity now covers this: no reallocation, no abort
  pool.push_back('\0');
  return true;
}

}  // namespace TextPool
