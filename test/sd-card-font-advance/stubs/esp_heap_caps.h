#pragma once
// Host-test stub for ESP-IDF's esp_heap_caps.h. The parser reads the largest contiguous
// block only to annotate its soft-flush and LAYOUT-OOM traces, so a fixed generous value
// keeps those paths on their normal branch: the host has no fragmentation to model, and a
// small number here would make the tests exercise the low-heap degradation instead.
#include <cstddef>

#define MALLOC_CAP_8BIT 0x04

inline size_t heap_caps_get_largest_free_block(unsigned) { return 512u * 1024u; }
