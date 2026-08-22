#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  bool parseCssFiles() const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  // Throw the cached CSS rules away and parse the stylesheets out of the EPUB again. The
  // section caches go with them: their pagination was flowed against the old rules, so keeping
  // them would reproduce the old layout no matter how good the new stylesheet is.
  // Callers holding an open section must release it first — SdFat cannot remove a directory
  // while a file inside it is open.
  bool rebuildCssCache();
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  // allowEarlyStop: a short write from `out` is treated as a polite stop (used by
  // header probes that need only the first bytes) rather than a write failure (#2611).
  // outStreamReason (optional) receives the underlying ZipFile::StreamResult cast
  // to uint8_t, so callers can report WHICH stream sub-failure occurred without
  // pulling ZipFile.h into this header. Map via ZipFile::streamResultTag().
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                                uint8_t* outStreamReason = nullptr) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
