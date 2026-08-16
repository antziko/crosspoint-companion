#include "OpdsStringArena.h"

#include <Memory.h>

#include <cstring>

const char* OpdsStringArena::add(const char* text, const size_t len) {
  // An empty field is the common case for `author` on navigation entries. Handing back a
  // static "" keeps it out of the arena entirely and still gives callers a valid C string.
  if (text == nullptr || len == 0) return "";

  const size_t need = len + 1;  // + NUL

  // Never split a string across chunks: the whole point of the arena is that the pointer
  // handed back is a usable C string, and a split one would not be. A string longer than a
  // chunk gets a chunk sized to it.
  Chunk* target = nullptr;
  if (!chunks.empty()) {
    Chunk& last = chunks.back();
    if (static_cast<size_t>(last.size) - last.used >= need) target = &last;
  }

  if (target == nullptr) {
    if (chunks.size() >= MAX_CHUNKS) return nullptr;
    const size_t chunkBytes = need > CHUNK_BYTES ? need : CHUNK_BYTES;
    auto data = makeUniqueNoThrow<char[]>(chunkBytes);
    if (!data) return nullptr;
    // reserve() allocates through the throwing operator new, which abort()s the device
    // under -fno-exceptions. reserveNoThrow probes first, so a heap too tight to hold one
    // more chunk handle degrades to a truncated feed instead of a reboot.
    if (!reserveNoThrow(chunks, chunks.size() + 1)) return nullptr;
    Chunk chunk;
    chunk.data = std::move(data);
    chunk.size = static_cast<uint16_t>(chunkBytes);
    chunk.used = 0;
    chunks.push_back(std::move(chunk));
    target = &chunks.back();
  }

  char* const dst = target->data.get() + target->used;
  memcpy(dst, text, len);
  dst[len] = '\0';
  target->used = static_cast<uint16_t>(target->used + need);
  return dst;
}

void OpdsStringArena::clear() {
  // Swap, not clear(): clear() destroys the chunks but keeps the vector's capacity block,
  // and reclaiming every byte is the entire reason a feed is released. Same reasoning as
  // OpdsBookBrowserActivity::releaseEntries().
  std::vector<Chunk>().swap(chunks);
}

size_t OpdsStringArena::bytesUsed() const {
  size_t total = 0;
  for (const Chunk& chunk : chunks) total += chunk.used;
  return total;
}
