#pragma once
// Host-test stub for ESP-IDF's esp_system.h. Free heap is trace-only in the parser; see
// esp_heap_caps.h for why the value is deliberately generous.
#include <cstdint>

inline uint32_t esp_get_free_heap_size() { return 512u * 1024u; }
