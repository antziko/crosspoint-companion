#pragma once

#include <BufferedFile.h>
#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool loaded;
  bool buildMode;

  HalFile bookFile;
  // Temp file handles during build
  HalFile spineFile;
  HalFile tocFile;
  // Buffers the per-entry tmp-file writes during the OPF/TOC passes: those
  // writes interleave with zip-inflate SD reads, and unbuffered they thrash
  // SdFat's shared sector cache (one 512B transaction per 4-byte pod). One
  // wrapper serves whichever pass is active (spine, then toc).
  std::unique_ptr<serialization::BufferedFileWriter> passOut;

  // Cumulative spine sizes, cached in RAM at load() so progress/percent lookups are
  // O(1) instead of 2 seeks + a heap-allocating SpineEntry read per access (4 bytes
  // per spine item).
  //
  // Deliberately NOT a std::vector: spineCount is uint16_t and 2000+ chapter EPUBs are
  // a documented case here (see the #134 note in finalize()), so this asks for up to
  // 8KB contiguous on a real book and 256KB in the limit. std::vector::reserve goes
  // through the *throwing* operator new, which -fno-exceptions turns into abort() — a
  // failed reserve would crash on book open rather than degrade. makeUniqueNoThrow
  // returns null instead, and callers fall back to the per-access seek path.
  std::unique_ptr<uint32_t[]> cumulativeSizes;
  uint16_t cumulativeSizeCount = 0;

  // Index for fast href→spineIndex lookup (used only for large EPUBs)
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::deque<SpineHrefIndexEntry> spineHrefIndex;
  bool useSpineHrefIndex = false;

  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;

  // FNV-1a 64-bit hash function
  static uint64_t fnvHash64(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  uint32_t writeSpineEntry(HalFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(HalFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(HalFile& file) const;
  TocEntry readTocEntry(HalFile& file) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)), lutOffset(0), spineCount(0), tocCount(0), loaded(false), buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  // Cumulative byte size up to and including the given spine item, from the in-RAM
  // cache populated in load(). False when the cache is absent (allocation failed) or
  // the index is out of range, so the caller can fall back to reading from the file.
  bool tryGetCumulativeSize(int index, uint32_t& out) const;
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
};
