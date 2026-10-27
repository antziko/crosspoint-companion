#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Publication years for the file browser, keyed by book path + file size, so a year is read
// out of an EPUB once and a replaced file (new size) is read again. Append-only records in
// /.crosspoint/book_years.bin; the last record for a key wins.
namespace BookYearCache {

// Year still to be read from the book.
constexpr int16_t YEAR_UNKNOWN = -1;

struct Query {
  uint32_t pathHash;
  uint32_t size;
  int16_t year;  // in: YEAR_UNKNOWN; out: the cached year (0 = book has none), else unchanged
};

uint32_t pathHash(const std::string& path);
// Fills every query found in the cache with one pass over the file.
void lookup(Query* queries, size_t count);
void store(uint32_t pathHash, uint32_t size, uint16_t year);

}  // namespace BookYearCache
