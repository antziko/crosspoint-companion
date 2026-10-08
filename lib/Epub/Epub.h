#pragma once

#include <ContentProtection.h>
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
  // .txt / .md: the book is converted into XHTML part files in the cache dir, and every
  // item read (parts, images, cover) comes from SD instead of a zip.
  bool textBook = false;
  // Optional encrypted-entry accessor. Entries are decoded in memory and stay
  // encrypted at rest. Null when the accessor is not needed or unavailable.
  std::unique_ptr<freeink::content::ContentDecryptor> decryptor;
  // User-presentable reason the encrypted-entry accessor could not be opened.
  std::string protectionError;
  // Epoch seconds a protected book's loan ends; 0 = not on loan.
  int64_t loanExpiresAt = 0;

  bool openProtection();

  bool findContentOpfFile(std::string* contentOpfFile, ZipFile* sharedZip = nullptr) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       bool metadataOnly = false, ZipFile* sharedZip = nullptr);
  // thumbPath / targetWidth / targetHeight / crop are resolved by the public entry points
  // below, so the cover-fit and contain-fit variants share one decode path.
  bool generateThumbBmpForCover(const std::string& thumbPath, int targetWidth, int targetHeight, bool crop,
                                const std::string& coverImageHref) const;
  // readFailed: the TOC document exists but its contents could not be read
  // (e.g. a decrypt or inflate failure under heap pressure), as opposed to
  // being absent or unparseable.
  bool parseTocNcxFile(bool* readFailed) const;
  bool parseTocNavFile(bool* readFailed) const;
  void discoverCssFilesFromZip();
  bool parseCssFiles() const;
  bool loadTextBook(bool buildIfMissing);
  bool buildTextBookCache();
  bool textBookStampMatches() const;
  std::string textItemPath(const std::string& itemHref) const;
  std::string findCompanionCover() const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool isTextBook() const { return textBook; }
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
  int64_t getLoanExpiresAt() const { return loanExpiresAt; }
  // Empty unless the encrypted-entry accessor failed to open.
  const std::string& getProtectionError() const { return protectionError; }
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  // Contain-fit thumbnail: the WHOLE cover scaled to fit inside width x height, so a tile of
  // that size draws it with neither a crop nor a downscale. Distinct cache file
  // (thumb_<w>x<h>.bmp) from the cover-fit thumb_<h>.bmp, which fills its box and overflows
  // it on one axis -- the two are not interchangeable and must not share a name.
  std::string getThumbFitBmpPath(int width, int height) const;
  bool generateThumbFitBmp(int width, int height) const;
  // Forget this session's failed-thumbnail blocklist so a cover that could not be decoded
  // under heap pressure is attempted again. Needed when the user explicitly asks to rebuild
  // covers: without it a rebuild silently skips exactly the books that most need one.
  static void forgetFailedThumbs();
  // Locate the cover without building spine, TOC, or reading caches.
  bool generateThumbBmpFromSource(int height);
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
