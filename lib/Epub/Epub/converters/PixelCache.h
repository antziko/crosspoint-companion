#pragma once

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <stdint.h>

#include <cstring>
#include <string>

// Cache buffer for storing 2-bit pixels (4 levels) during decode.
// Packs 4 pixels per byte, MSB first.
struct PixelCache {
  uint8_t* buffer;
  int width;
  int height;
  int bytesPerRow;
  int originX;  // config.x - to convert screen coords to cache coords
  int originY;  // config.y

  PixelCache() : buffer(nullptr), width(0), height(0), bytesPerRow(0), originX(0), originY(0) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr size_t MAX_CACHE_BYTES = 256 * 1024;  // 256KB limit for embedded targets

  bool allocate(int w, int h, int ox, int oy) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    size_t bufferSize = (size_t)bytesPerRow * h;
    if (bufferSize > MAX_CACHE_BYTES) {
      LOG_ERR("IMG", "Cache buffer too large: %d bytes for %dx%d (limit %d)", bufferSize, w, h, MAX_CACHE_BYTES);
      return false;
    }
    buffer = (uint8_t*)malloc(bufferSize);
    if (buffer) {
      memset(buffer, 0, bufferSize);
      LOG_DBG("IMG", "Allocated cache buffer: %d bytes for %dx%d", bufferSize, w, h);
    }
    return buffer != nullptr;
  }

  void setPixel(int screenX, int screenY, uint8_t value) {
    if (!buffer) return;
    int localX = screenX - originX;
    int localY = screenY - originY;
    if (localX < 0 || localX >= width || localY < 0 || localY >= height) return;

    int byteIdx = localY * bytesPerRow + localX / 4;
    int bitShift = 6 - (localX % 4) * 2;  // MSB first: pixel 0 at bits 6-7
    buffer[byteIdx] = (buffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  bool writeToFile(const std::string& cachePath) {
    if (!buffer) return false;

    HalFile cacheFile;
    if (!Storage.openFileForWrite("IMG", cachePath, cacheFile)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      return false;
    }

    uint16_t w = width;
    uint16_t h = height;
    cacheFile.write(&w, 2);
    cacheFile.write(&h, 2);
    cacheFile.write(buffer, bytesPerRow * height);
    cacheFile.close();

    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePath.c_str(), width, height, 4 + bytesPerRow * height);
    return true;
  }

  ~PixelCache() {
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }
};

// Streaming variant for images whose full 2-bit buffer won't fit in RAM (e.g. a
// full-page cover at ~80KB on a device whose largest free block is ~63KB). Holds
// only a small rolling band of rows; as the decoder advances past a row it is
// flushed to SD. Produces the exact same .px13n file as PixelCache::writeToFile
// (header [w][h] then 2bpp rows, top to bottom), so the reader is unchanged.
//
// Correctness relies on the decoder delivering output rows in non-decreasing
// order (baseline JPEG raster MCU order). The caller flushes rows strictly below
// the current block's top via flushBelow() — never a row a later block in the
// same MCU-row could still touch. Progressive JPEGs (non-raster) must NOT use
// this; the converter falls back to no-cache for them.
struct StreamingPixelCache {
  HalFile file;
  uint8_t* band{nullptr};
  int bytesPerRow{0};
  int width{0};
  int height{0};
  int originX{0};       // config.x — screen X of cache column 0
  int bandRows{0};      // band capacity in rows
  int bandStartRow{0};  // local row index held at band[0]
  int writtenRows{0};   // next local row to flush to file (flush cursor)
  bool ok{false};
  bool borrowedScratch{false};  // band points at the shared inflate window, not malloc
  uint8_t* rowPtr{nullptr};      // current row within band (set by beginRow), or null

  StreamingPixelCache() = default;
  StreamingPixelCache(const StreamingPixelCache&) = delete;
  StreamingPixelCache& operator=(const StreamingPixelCache&) = delete;

  // Release the band buffer however it was obtained (borrowed window vs malloc).
  void releaseBand() {
    if (!band) return;
    if (borrowedScratch) {
      InflateReader::releaseScratch();
      borrowedScratch = false;
    } else {
      free(band);
    }
    band = nullptr;
  }

  // Open the cache file, write the header, allocate the band. bandRowsCap must
  // exceed the dest-row height of one decoded block (a few rows; 128 is ample).
  bool begin(const std::string& cachePath, int w, int h, int ox, int bandRowsCap) {
    width = w;
    height = h;
    originX = ox;
    bandRows = bandRowsCap;
    bytesPerRow = (w + 3) / 4;
    const size_t bandBytes = (size_t)bytesPerRow * bandRows;
    band = (uint8_t*)malloc(bandBytes);
    if (!band) {
      // Heap too fragmented for the band — borrow the reserved 32KB inflate window
      // (free here: JPEG decode doesn't inflate). Keeps the streaming cache working
      // under pressure instead of falling back to a slow multi-decode, no-cache page.
      band = InflateReader::acquireScratch(bandBytes);
      borrowedScratch = (band != nullptr);
      if (!band) {
        LOG_ERR("IMG", "Streaming cache: band alloc failed (%zu bytes)", bandBytes);
        return false;
      }
    }
    memset(band, 0, bandBytes);
    if (!Storage.openFileForWrite("IMG", cachePath, file)) {
      LOG_ERR("IMG", "Streaming cache: open failed: %s", cachePath.c_str());
      releaseBand();
      return false;
    }
    uint16_t w16 = (uint16_t)w;
    uint16_t h16 = (uint16_t)h;
    file.write(&w16, 2);
    file.write(&h16, 2);
    bandStartRow = 0;
    writtenRows = 0;
    rowPtr = nullptr;
    ok = true;
    return true;
  }

  // Flush every band row strictly below watermarkLocalRow (those rows are final —
  // they belong to fully-decoded MCU-rows), then slide the band up.
  void flushBelow(int watermarkLocalRow) {
    if (!ok) return;
    while (writtenRows < watermarkLocalRow && writtenRows < height) {
      const int bandIdx = writtenRows - bandStartRow;
      if (bandIdx < 0 || bandIdx >= bandRows) return;  // unexpected; leave for finish() to detect
      file.write(band + (size_t)bandIdx * bytesPerRow, bytesPerRow);
      writtenRows++;
    }
    const int shift = writtenRows - bandStartRow;
    if (shift <= 0) return;
    if (shift >= bandRows) {
      memset(band, 0, (size_t)bytesPerRow * bandRows);
    } else {
      memmove(band, band + (size_t)shift * bytesPerRow, (size_t)(bandRows - shift) * bytesPerRow);
      memset(band + (size_t)(bandRows - shift) * bytesPerRow, 0, (size_t)shift * bytesPerRow);
    }
    bandStartRow = writtenRows;
  }

  inline void beginRow(int screenY, int cacheOriginY) {
    const int localRow = screenY - cacheOriginY;
    const int bandIdx = localRow - bandStartRow;
    if (!ok || bandIdx < 0 || bandIdx >= bandRows) {
      if (bandIdx >= bandRows) ok = false;  // block taller than the band — abort caching
      rowPtr = nullptr;
      return;
    }
    rowPtr = band + (size_t)bandIdx * bytesPerRow;
  }

  inline void writePixel(int screenX, uint8_t value) const {
    if (!rowPtr) return;
    const int localX = screenX - originX;
    const int byteIdx = localX >> 2;
    const int bitShift = 6 - (localX & 3) * 2;  // MSB first
    rowPtr[byteIdx] = (rowPtr[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  // Flush the rest and close. Returns true only if the whole image was written.
  bool finish() {
    if (!ok) {
      releaseBand();
      return false;
    }
    flushBelow(height);
    file.close();
    const bool complete = (writtenRows >= height);
    releaseBand();
    ok = false;
    return complete;
  }

  ~StreamingPixelCache() { releaseBand(); }
};
