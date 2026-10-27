#include "BookYearCache.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace BookYearCache {
namespace {
constexpr char CACHE_PATH[] = "/.crosspoint/book_years.bin";
// hash u32, size u32, year u16, pad u16
constexpr size_t RECORD_SIZE = 12;
constexpr size_t RECORDS_PER_READ = 32;
// Past this the file is restarted rather than compacted: ~5000 books, and rereading a year is cheap.
constexpr size_t MAX_FILE_SIZE = 60 * 1024;
}  // namespace

uint32_t pathHash(const std::string& path) {
  uint32_t h = 2166136261u;  // FNV-1a 32: stable across builds, unlike std::hash
  for (const char c : path) {
    h ^= static_cast<uint8_t>(c);
    h *= 16777619u;
  }
  return h;
}

void lookup(Query* queries, const size_t count) {
  if (count == 0) return;
  HalFile file;
  if (!Storage.exists(CACHE_PATH) || !Storage.openFileForRead("BYC", CACHE_PATH, file)) return;
  uint8_t buf[RECORD_SIZE * RECORDS_PER_READ];
  int n;
  while ((n = file.read(buf, sizeof(buf))) >= static_cast<int>(RECORD_SIZE)) {
    for (size_t off = 0; off + RECORD_SIZE <= static_cast<size_t>(n); off += RECORD_SIZE) {
      uint32_t hash;
      uint32_t size;
      uint16_t year;
      memcpy(&hash, buf + off, sizeof(hash));
      memcpy(&size, buf + off + 4, sizeof(size));
      memcpy(&year, buf + off + 8, sizeof(year));
      for (size_t i = 0; i < count; ++i) {
        if (queries[i].pathHash == hash && queries[i].size == size) queries[i].year = static_cast<int16_t>(year);
      }
    }
  }
}

void store(const uint32_t pathHash, const uint32_t size, const uint16_t year) {
  HalFile file;
  if (!Storage.openFileForAppend("BYC", CACHE_PATH, file)) return;
  if (file.size() >= MAX_FILE_SIZE) {
    file.close();  // close before remove and reopen
    Storage.remove(CACHE_PATH);
    if (!Storage.openFileForAppend("BYC", CACHE_PATH, file)) return;
  }
  uint8_t rec[RECORD_SIZE] = {};
  memcpy(rec, &pathHash, sizeof(pathHash));
  memcpy(rec + 4, &size, sizeof(size));
  memcpy(rec + 8, &year, sizeof(year));
  if (file.write(rec, sizeof(rec)) != sizeof(rec)) LOG_ERR("BYC", "Short write to %s", CACHE_PATH);
}

}  // namespace BookYearCache
