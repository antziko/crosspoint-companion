#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "VectorFontSupport.h"

// Longest "<base>_<size>.cpfont" the registry will admit. Files whose name does not
// fit are skipped at scan time rather than truncated — a truncated name would build
// a path that either fails to open or, worse, opens a different file.
inline constexpr size_t SD_FONT_FILENAME_MAX = 64;

struct SdCardFontFileInfo {
  // Leaf filename only, e.g. "NotoSansCJK_14.cpfont". Stored verbatim rather than
  // derived: the base need not match the family directory (parseFilename admits any
  // "<anything>_<size>.cpfont", e.g. "Bookerly-SD_14.cpfont" inside "Bookerly/").
  // Inline rather than std::string so a family's file list costs no per-file heap
  // block — see SdCardFontRegistry::familyWithFiles.
  char filename[SD_FONT_FILENAME_MAX];
  uint8_t pointSize;  // parsed from filename: 14 (0 for size-free vector fonts)
  uint8_t style;      // .cpfont: always 0 in v4 (all 4 styles bundled in one file).
                      // Vector family: the style ROLE of this file --
                      // 0=regular, 1=bold, 2=italic, 3=bold-italic.
};

struct SdCardFontFamilyInfo {
  std::string name;        // directory name, e.g. "NotoSansCJK"
  bool hiddenRoot = true;  // which root holds it: /.fonts when true, /fonts when false
  // true for a folder of TrueType/OpenType files (.ttf/.otf/.ttc) rendered at any size via
  // the FreeInkFont engine (see TtfEpdFont / SdCardFontSystem); false for a folder of
  // pre-rasterized .cpfont files. Vector entries carry pointSize 0 and a style ROLE.
  // Retained on the catalogue entry, not just the scratch listing, because the font picker
  // asks which sizes a family offers before any listing is materialized.
  bool vector = false;
  // Empty on entries owned by SdCardFontRegistry::families_ — the registry stores the
  // catalogue (name + root), not the listing. Populated only on the scratch entry
  // familyWithFiles() fills, and on any local a caller scans into.
  std::vector<SdCardFontFileInfo> files;

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  // Installed file closest to `pointSize` (ties → smaller). nullptr when the
  // family ships nothing in `style`.
  const SdCardFontFileInfo* findNearestSize(uint8_t pointSize, uint8_t style = 0) const;
  std::vector<uint8_t> availableSizes() const;
};

class SdCardFontRegistry {
 public:
  static constexpr int MAX_SD_FAMILIES = 128;
  // Two top-level roots are scanned at discovery time. Hidden is preferred
  // when creating new installs; both are read from if present.
  static constexpr const char* FONTS_DIR_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_VISIBLE = "/fonts";

  // Returns the existing root for `familyName` (the one that contains
  // /<root>/<familyName>/), or nullptr if the family is not installed in
  // either root. Used by writers to keep re-installs in their existing dir.
  static const char* findFamilyRoot(const char* familyName);

  // Returns the root path that should be used when creating a brand-new
  // family on disk (no prior install): the existing root if exactly one of
  // the two roots exists, otherwise the hidden root.
  static const char* defaultWriteRoot();

  static const char* rootFor(bool hidden) { return hidden ? FONTS_DIR_HIDDEN : FONTS_DIR_VISIBLE; }

  // "<root>/<family>/<filename>" into `outBuf`.
  static void buildPath(const SdCardFontFamilyInfo& family, const SdCardFontFileInfo& file, char* outBuf,
                        size_t outBufSize);

  // Scan SD card, populate families_ (names + roots only). Returns true if any
  // families found. Invalidates the file cache.
  bool discover();

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

  // The family plus its file listing, scanned from SD on demand and cached for the
  // most recent family. nullptr when the family is not in the catalogue.
  //
  // One entry, because the callers are one-family-at-a-time by construction: only one
  // SD family is resident at a time (SdCardFontManager::loadFamily unloads first), and
  // the size lists / nearest-size lookups all ask about that same family. The returned
  // pointer is INVALIDATED by the next call with a different name, and by discover() —
  // use it before asking for another family, never store it.
  const SdCardFontFamilyInfo* familyWithFiles(const std::string& name) const;

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically; files empty

  // Cache for familyWithFiles(). Mutable so the accessor can stay const: it is a cache
  // of what is on the card, not registry state. Single-task by construction — the web
  // server's handleClient() runs on the activity loop, not its own task.
  mutable SdCardFontFamilyInfo fileCache_;
  mutable bool fileCacheValid_ = false;
  void invalidateFileCache() const;

  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
#if CROSSPOINT_VECTOR_FONTS
  // Match a vector font filename (.ttf/.otf/.ttc, case-insensitive) and return the length of
  // the base name (extension stripped) in `baseLen`.
  static bool parseVectorFontName(const char* filename, size_t& baseLen);
  // Style role (0=regular, 1=bold, 2=italic, 3=bold-italic) inferred from a vector font's
  // base name (case-insensitive "bold"/"italic"/"oblique" tokens).
  static uint8_t parseVectorStyle(const char* baseName, size_t baseLen);
  // Refine each vector file's style role from its real face metadata (FtFont::inspectStream:
  // OS/2 weight + italic flag), keeping the filename-derived role when the face cannot be
  // read. Then dedup by role.
  static void refineVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files);
#endif
  // Scan one family directory, filling `family.files`.
  static void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
  // True when `dirPath` holds at least one admissible .cpfont. Used at discovery so a
  // directory can be admitted without retaining its listing.
  // `outVector` reports which kind was found (.cpfont wins when a folder holds both).
  static bool hasAnyFontFile(const char* dirPath, bool* outVector = nullptr);
#if CROSSPOINT_VECTOR_FONTS
 public:
  // FtFont::ReadFn over a HalFile* ctx (absolute-offset reads; count 0 is a seek probe).
  // Shared by face inspection here and the streamed TTF sources in SdCardFontSystem.
  static unsigned long halFileRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);

 private:
#endif
  // Scan one root (e.g. "/.fonts"), append families to `out`, dedup by name.
  static void scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out);
};
