#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
};

// Loads reader progress for an EPUB from its cache directory. Returns true on success.
// Mirrors the read-side parsing in EpubReaderActivity::onEnter (4-or-6-byte format,
// UINT16_MAX "previous chapter, last page" navigation sentinel never persisted as resume state).
inline bool loadProgress(Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  HalFile f;
  if (!Storage.openFileForRead(moduleName, epub.getCachePath() + "/progress.bin", f)) {
    return false;
  }
  uint8_t data[6];
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize != 4 && dataSize != 6) {
    return false;
  }
  progress.spineIndex = data[0] | (data[1] << 8);
  progress.pageNumber = data[2] | (data[3] << 8);
  if (progress.pageNumber == UINT16_MAX) {
    progress.pageNumber = 0;
  }
  if (dataSize == 6) {
    progress.pageCount = data[4] | (data[5] << 8);
    progress.hasPageCount = true;
  } else {
    progress.pageCount = 0;
    progress.hasPageCount = false;
  }
  return true;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("ERS", epub.getCachePath() + "/progress.bin", f)) {
    LOG_ERR("ERS", "Could not open progress file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);
  return true;
}

}  // namespace EpubReaderUtils
