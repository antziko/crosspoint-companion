#pragma once

#include <cstdint>

// Packed codepoint + occurrence count, for SdCardFont::prewarm's dedup buffer.
//
// prewarm() records how many times each codepoint appears on the page so prewarmStyle() can
// decide *which* glyphs to keep when the page's bitmaps do not all fit its arena budget.
// Chinese is strongly Zipf-distributed, so which glyphs survive matters far more than how
// many: a CJK page wants 25-38 KB of bitmaps on a heap whose largest free block while
// reading is 7-15 KB.
//
// The count rides in the high bits of the same uint32_t rather than in a parallel array,
// because that buffer is itself an observed OOM site on this heap (a failed 2048-byte
// allocation against a 2036-byte largest free block); widening it would make the worst case
// worse. Unicode tops out at U+10FFFF, so 21 bits hold every legal codepoint and the
// remaining 11 hold a saturating count.
//
// Header-only and pure — no allocation, logging or I/O — so it can be unit-tested on the
// host without linking the SD/Arduino stack.
namespace CodepointFreq {

inline constexpr uint32_t VALUE_BITS = 21;
inline constexpr uint32_t VALUE_MASK = (1u << VALUE_BITS) - 1;  // 0x1FFFFF
inline constexpr uint32_t FREQ_ONE = 1u << VALUE_BITS;
inline constexpr uint32_t FREQ_MAX = (1u << (32 - VALUE_BITS)) - 1;  // 2047, then saturates
inline constexpr uint32_t MAX_CODEPOINT = 0x10FFFF;

static_assert(MAX_CODEPOINT <= VALUE_MASK, "21 value bits must cover the whole Unicode range");

inline constexpr uint32_t value(const uint32_t packed) { return packed & VALUE_MASK; }
inline constexpr uint32_t freq(const uint32_t packed) { return packed >> VALUE_BITS; }

// First sighting of a codepoint: count 1.
inline constexpr uint32_t pack(const uint32_t codepoint) { return (codepoint & VALUE_MASK) | FREQ_ONE; }

// Pack with an explicit count, clamped. Used for entries that must outrank the page's own
// text — the replacement glyph, which is the fallback for everything that fails to render.
inline constexpr uint32_t packWith(const uint32_t codepoint, const uint32_t count) {
  const uint32_t c = count > FREQ_MAX ? FREQ_MAX : count;
  return (codepoint & VALUE_MASK) | (c << VALUE_BITS);
}

// Another sighting. Saturates rather than wrapping into the codepoint bits.
inline constexpr uint32_t bump(const uint32_t packed) { return freq(packed) >= FREQ_MAX ? packed : packed + FREQ_ONE; }

}  // namespace CodepointFreq
