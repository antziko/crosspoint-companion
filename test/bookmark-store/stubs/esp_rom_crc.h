#pragma once
#include <cstddef>
#include <cstdint>

// Host stub for the ESP-ROM CRC32. Only determinism matters for the tests (the value
// is used to derive per-book cache filenames); this is a standard CRC-32/ISO-HDLC.
inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len) {
  crc = ~crc;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1) - 1)));
    }
  }
  return ~crc;
}
