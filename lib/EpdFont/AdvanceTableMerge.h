#pragma once

#include <cstdint>

// Pure sorted-array merge helpers behind SdCardFont's persistent advance table.
//
// Split out of SdCardFont so the arithmetic is host-testable without Arduino/HAL (see
// test/sd-card-font-advance). No allocation, no logging, no I/O — the caller owns all buffers.
//
// The table is merged a few (often ONE) codepoint at a time, because buildAdvanceTable only fetches
// what is missing. Reallocating per merge cost ~700 allocate-copy-free cycles per style to reach the
// cap and was the session-long heap fragmenter on X3. These helpers let the caller keep spare
// capacity and merge in place, so the steady state touches the allocator zero times.
//
// Entry is any struct with a `uint32_t codepoint` member. Instantiated once in firmware
// (SdCardFont::AdvanceEntry), so the template costs no extra binary size.
namespace AdvanceTableMerge {

// Size the merge result would have, with `limit` truncating the tail.
inline uint32_t mergedSize(const uint32_t oldSize, const uint32_t newCount, const uint32_t limit) {
  const uint32_t total = oldSize + newCount;
  return total > limit ? limit : total;
}

// Capacity to allocate when `needed` no longer fits `currentCap`. Geometric (doubling from minCap)
// so a style reallocates a handful of times per session instead of once per merge, clamped to
// `limit` and never below `needed`.
inline uint32_t nextCapacity(const uint32_t currentCap, const uint32_t needed, const uint32_t minCap,
                             const uint32_t limit) {
  uint32_t cap = currentCap ? currentCap * 2 : minCap;
  if (cap < needed) cap = needed;
  if (cap > limit) cap = limit;
  return cap;
}

// Merge `b` (newCount, sorted by codepoint, no overlap with `a`) into `a` (oldSize, sorted) in
// place, writing `needed` entries. `a` must have room for `needed`. Truncation drops the LARGEST
// codepoints, matching mergeForward.
//
// Walks both tails downwards. The write index stays at (read index into a) + (read index into b) +
// 1, so it leads `i` until `b` is exhausted, after which the remaining copies are self-assignments
// — an unread entry of `a` can never be clobbered.
template <typename Entry>
void mergeInPlace(Entry* a, const uint32_t oldSize, const Entry* b, const uint32_t newCount, const uint32_t needed) {
  int64_t i = static_cast<int64_t>(oldSize) - 1;
  int64_t j = static_cast<int64_t>(newCount) - 1;

  // Discard the truncated tail before writing anything.
  for (uint32_t excess = (oldSize + newCount) - needed; excess > 0; excess--) {
    if (j >= 0 && (i < 0 || b[j].codepoint > a[i].codepoint)) {
      j--;
    } else {
      i--;
    }
  }

  for (int64_t k = static_cast<int64_t>(needed) - 1; k >= 0; k--) {
    if (j >= 0 && (i < 0 || b[j].codepoint > a[i].codepoint)) {
      a[k] = b[j--];
    } else {
      a[k] = a[i--];
    }
  }
}

// Merge `a` and `b` into a distinct `dst` of at least `needed` entries, stopping at `needed`
// (dropping the largest codepoints). Returns the number written. Used on the grow path, where a
// fresh buffer has just been allocated.
template <typename Entry>
uint32_t mergeForward(Entry* dst, const uint32_t needed, const Entry* a, const uint32_t oldSize, const Entry* b,
                      const uint32_t newCount) {
  uint32_t i = 0, j = 0, k = 0;
  while (k < needed && (i < oldSize || j < newCount)) {
    if (i < oldSize && (j >= newCount || a[i].codepoint <= b[j].codepoint)) {
      dst[k++] = a[i++];
    } else {
      dst[k++] = b[j++];
    }
  }
  return k;
}

}  // namespace AdvanceTableMerge
