#include "SdCardFontRegistry.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::findNearestSize(const uint8_t pointSize, const uint8_t style) const {
  // The reader stores an actual point size, so an exact match is the norm and
  // falls out of the delta search below (delta 0). The search only matters when
  // the size was carried over from a family that ships different sizes; the
  // caller then persists the snapped size (SdCardFontSystem::ensureLoaded).
  const SdCardFontFileInfo* best = nullptr;
  uint8_t bestDelta = 255;
  for (const auto& f : files) {
    if (f.style != style) continue;
    const uint8_t delta = f.pointSize > pointSize ? f.pointSize - pointSize : pointSize - f.pointSize;
    // Ties resolve to the smaller size, matching snapToNearestPointSize().
    if (!best || delta < bestDelta || (delta == bestDelta && f.pointSize < best->pointSize)) {
      best = &f;
      bestDelta = delta;
    }
  }
  return best;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  // V4 .cpfont files bundle every style (regular/bold/italic/bold-italic) into
  // one file, so style is always 0 at the registry level. The per-style
  // bitstream is selected later by SdCardFont::getEpdFont(style). The `style`
  // field in SdCardFontFileInfo is reserved for future formats that split
  // styles across files; scanDirectory() defends against accidental
  // (pointSize, style) collisions in that scenario.
  style = 0;
  return true;
}

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t size, style;
    if (!parseFilename(nameBuffer, size, style)) continue;

    // Reject duplicate (pointSize, style) entries in the same family. With
    // v4's bundle-everything design parseFilename always returns style=0, so
    // two files at the same size in the same family would silently shadow
    // each other in findFile(). Skip the duplicate and warn.
    bool duplicate = false;
    for (const auto& existing : family.files) {
      if (existing.pointSize == size && existing.style == style) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", nameBuffer, dirPath);
      continue;
    }

    // Bounded copy, and SKIP rather than truncate: a truncated name builds a path
    // that either fails to open or names a different file.
    if (strlen(nameBuffer) >= SD_FONT_FILENAME_MAX) {
      LOG_ERR("SDREG", "Font name too long (%u >= %u), skipping: %s", (unsigned)strlen(nameBuffer),
              (unsigned)SD_FONT_FILENAME_MAX, nameBuffer);
      continue;
    }

    SdCardFontFileInfo info{};
    std::strcpy(info.filename, nameBuffer);
    info.pointSize = size;
    info.style = style;
    family.files.push_back(info);
  }
}

// Discovery only needs to know whether a directory holds at least one admissible
// .cpfont (scanRoot's admission rule). Answering that without materialising the
// listing is what keeps the catalogue to a name and a root per family.
bool SdCardFontRegistry::hasAnyFontFile(const char* dirPath) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
    if (strlen(nameBuffer) >= SD_FONT_FILENAME_MAX) continue;
    uint8_t size, style;
    if (parseFilename(nameBuffer, size, style)) return true;
  }
  return false;
}

// Scan a single root (e.g. "/.fonts") and append its families to `out`.
// Skips families whose names already exist in `out` (de-duplicates between
// the hidden and visible roots — first scan wins).
void SdCardFontRegistry::scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    return;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip hidden/system directories inside the root (macOS ._*, .Trashes, etc.)
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

      // De-dup by family name across roots.
      bool exists = false;
      for (const auto& fam : out) {
        if (fam.name == nameBuffer) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      char subDirPath[192];
      snprintf(subDirPath, sizeof(subDirPath), "%s/%s", rootPath, nameBuffer);
      // The admission rule is unchanged (a directory with no usable .cpfont is not a
      // family); only the listing is discarded rather than retained.
      if (!hasAnyFontFile(subDirPath)) continue;

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      family.hiddenRoot = (strcmp(rootPath, FONTS_DIR_HIDDEN) == 0);
      out.push_back(std::move(family));
      LOG_DBG("SDREG", "Found family: %s in %s", out.back().name.c_str(), rootPath);
    } else {
      entry.close();
    }
  }
}

bool SdCardFontRegistry::discover() {
  // Measurement, not decoration: the catalogue is resident for the whole session and
  // grows with every family installed, so its true cost on a real card is the number
  // that says whether the font screen's post-download reload has room. Blocks matter
  // as much as bytes — the 09-17 abort died asking for 31 bytes with 1,436 free,
  // because the free heap was in fragments of 28.
  multi_heap_info_t before;
  heap_caps_get_info(&before, MALLOC_CAP_8BIT);
  const uint32_t startMs = millis();

  invalidateFileCache();
  families_.clear();
  families_.reserve(MAX_SD_FAMILIES);

  // Hidden root is scanned first so it wins on name collisions, matching the
  // sleep-folder pattern (/.sleep preferred over /sleep).
  scanRoot(FONTS_DIR_HIDDEN, families_);
  scanRoot(FONTS_DIR_VISIBLE, families_);

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  multi_heap_info_t after;
  heap_caps_get_info(&after, MALLOC_CAP_8BIT);
  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  // The per-context trace flag is off during setup() — no screen has opened yet — and the
  // boot discovery is precisely the measurement that matters. Force it on for this one line
  // and restore; the user's master switch (SETTINGS.sdCardLogging) still gates it.
  const bool traceWasEnabled = SdDebugLog::isEnabled();
  SdDebugLog::setEnabled(true);
  SdDebugLog::log("SDREG", "discover families=%d ms=%lu free=%u->%u largest=%u->%u blocks=%u->%u",
                  static_cast<int>(families_.size()), (unsigned long)(millis() - startMs),
                  (unsigned)before.total_free_bytes, (unsigned)after.total_free_bytes,
                  (unsigned)before.largest_free_block, (unsigned)after.largest_free_block, (unsigned)before.free_blocks,
                  (unsigned)after.free_blocks);
  SdDebugLog::setEnabled(traceWasEnabled);
  return !families_.empty();
}

void SdCardFontRegistry::invalidateFileCache() const {
  fileCacheValid_ = false;
  fileCache_.name.clear();
  // swap-with-empty, not clear(): the capacity IS the block being handed back.
  std::vector<SdCardFontFileInfo>().swap(fileCache_.files);
}

const SdCardFontFamilyInfo* SdCardFontRegistry::familyWithFiles(const std::string& name) const {
  if (fileCacheValid_ && fileCache_.name == name) return &fileCache_;

  const SdCardFontFamilyInfo* entry = findFamily(name);
  if (!entry) return nullptr;

  invalidateFileCache();
  fileCache_.name = entry->name;
  fileCache_.hiddenRoot = entry->hiddenRoot;

  char dirPath[192];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", rootFor(entry->hiddenRoot), entry->name.c_str());
  scanDirectory(dirPath, fileCache_);
  if (fileCache_.files.empty()) {
    // Admitted at discovery but empty now (deleted under us). Leave the cache invalid so
    // the next call re-scans rather than serving an empty listing forever.
    invalidateFileCache();
    return nullptr;
  }
  fileCacheValid_ = true;
  return &fileCache_;
}

const char* SdCardFontRegistry::findFamilyRoot(const char* familyName) {
  if (!familyName || !*familyName) return nullptr;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_HIDDEN, familyName);
  if (Storage.exists(path)) return FONTS_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_VISIBLE, familyName);
  if (Storage.exists(path)) return FONTS_DIR_VISIBLE;
  return nullptr;
}

void SdCardFontRegistry::buildPath(const SdCardFontFamilyInfo& family, const SdCardFontFileInfo& file, char* outBuf,
                                   const size_t outBufSize) {
  snprintf(outBuf, outBufSize, "%s/%s/%s", rootFor(family.hiddenRoot), family.name.c_str(), file.filename);
}

const char* SdCardFontRegistry::defaultWriteRoot() {
  // If exactly one of the roots already exists, keep using it. Otherwise
  // (neither exists, or both exist) prefer the hidden root for new installs.
  bool hiddenExists = Storage.exists(FONTS_DIR_HIDDEN);
  bool visibleExists = Storage.exists(FONTS_DIR_VISIBLE);
  if (hiddenExists) return FONTS_DIR_HIDDEN;
  if (visibleExists) return FONTS_DIR_VISIBLE;
  return FONTS_DIR_HIDDEN;
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}
