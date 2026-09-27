#include "SdCardFontRegistry.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <esp_heap_caps.h>
#include <strings.h>  // strcasecmp / strncasecmp
#if CROSSPOINT_VECTOR_FONTS
#include <FtFont.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

#if CROSSPOINT_VECTOR_FONTS

bool SdCardFontRegistry::parseVectorFontName(const char* filename, size_t& baseLen) {
  static constexpr const char* kExts[] = {".ttf", ".otf", ".ttc"};
  const size_t nameLen = strlen(filename);
  for (const char* ext : kExts) {
    const size_t extLen = strlen(ext);
    if (nameLen <= extLen) continue;
    const char* tail = filename + nameLen - extLen;
    if (strcasecmp(tail, ext) == 0) {
      baseLen = nameLen - extLen;
      return baseLen > 0 && baseLen <= 127;
    }
  }
  return false;
}

uint8_t SdCardFontRegistry::parseVectorStyle(const char* baseName, const size_t baseLen) {
  // Case-insensitive token scan. "bold" (incl. semibold/demibold) -> bold bit;
  // "italic"/"oblique" -> italic bit. Anything else is regular.
  bool bold = false;
  bool ital = false;
  for (size_t i = 0; i < baseLen; ++i) {
    if ((baseLen - i) >= 4 && strncasecmp(baseName + i, "bold", 4) == 0) bold = true;
    if ((baseLen - i) >= 6 && strncasecmp(baseName + i, "italic", 6) == 0) ital = true;
    if ((baseLen - i) >= 7 && strncasecmp(baseName + i, "oblique", 7) == 0) ital = true;
  }
  return static_cast<uint8_t>((bold ? 1 : 0) | (ital ? 2 : 0));
}

unsigned long SdCardFontRegistry::halFileRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                              const unsigned long count) {
  auto* f = static_cast<HalFile*>(ctx);
  if (f == nullptr || !*f) return 0;
  if (!f->seek(static_cast<size_t>(offset))) return 0;
  if (count == 0) return 0;
  const int n = f->read(buffer, count);
  return n < 0 ? 0 : static_cast<unsigned long>(n);
}

void SdCardFontRegistry::refineVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files) {
  using freeink::font::FtFont;
  // Read each face's real weight + italic flag (inspectStream reads only the sfnt header
  // tables, no face is retained), then pick the four roles DETERMINISTICALLY by design
  // weight: the upright face nearest 400 is regular, nearest 700 is bold; same for the
  // italics. This is independent of SD directory order -- a Regular/Medium/Semibold/Bold/
  // Black family always resolves to Regular + Bold, not to whichever file enumerated first.
  // An unreadable face falls back to its filename-derived role (Regular/Bold -> 400/700).
  struct Candidate {
    size_t index;  // into files
    uint16_t weight;
    bool italic;
  };
  std::vector<Candidate> cands;
  cands.reserve(files.size());
  char pathBuf[256];
  for (size_t i = 0; i < files.size(); ++i) {
    Candidate c{i, static_cast<uint16_t>((files[i].style & 1) ? 700 : 400), (files[i].style & 2) != 0};
    snprintf(pathBuf, sizeof(pathBuf), "%s/%s", dirPath, files[i].filename);
    HalFile f = Storage.open(pathBuf);
    if (f && !f.isDirectory()) {
      FtFont::FaceInfo face;
      if (FtFont::inspectStream(&halFileRead, &f, static_cast<unsigned long>(f.size()), face) ==
          FtFont::InspectResult::Ok) {
        c.weight = face.weight;
        c.italic = face.italic;
      }
    }
    cands.push_back(c);
  }

  // Nearest target weight within the upright/italic bucket; ties break to the lower weight,
  // then the lexicographically smaller FILENAME -- never enumeration order. (Filename, not
  // full path: every candidate is in the same directory, so the two order identically.)
  // `exclude` keeps bold from re-picking the regular file.
  const auto pick = [&](const bool italic, const int target, const Candidate* exclude) -> const Candidate* {
    const Candidate* best = nullptr;
    for (const auto& c : cands) {
      if (c.italic != italic || &c == exclude) continue;
      if (!best) {
        best = &c;
        continue;
      }
      const int dc = std::abs(static_cast<int>(c.weight) - target);
      const int db = std::abs(static_cast<int>(best->weight) - target);
      if (dc < db || (dc == db && (c.weight < best->weight ||
                                   (c.weight == best->weight &&
                                    strcmp(files[c.index].filename, files[best->index].filename) < 0)))) {
        best = &c;
      }
    }
    return best;
  };

  const Candidate* regular = pick(false, 400, nullptr);
  if (!regular) {
    // All faces italic: the italic nearest 400 anchors the family as regular (TtfEpdFont
    // needs a regular source; it derives the rest).
    regular = pick(true, 400, nullptr);
    if (regular) LOG_DBG("SDREG", "No upright face in %s — promoting %s", dirPath, files[regular->index].filename);
    if (!regular) return;  // no usable files at all
  }
  // Bold must be a genuinely heavier face than the regular pick; otherwise the synthesizer
  // derives it (a same-or-lighter file would render identically).
  const Candidate* bold = pick(false, 700, regular);
  if (bold && bold->weight <= regular->weight) bold = nullptr;
  const Candidate* italic = regular->italic ? nullptr : pick(true, 400, nullptr);
  const Candidate* boldItalic = pick(true, 700, italic ? italic : regular);
  if (boldItalic && italic && boldItalic->weight <= italic->weight) boldItalic = nullptr;
  if (boldItalic && !boldItalic->italic) boldItalic = nullptr;

  std::vector<SdCardFontFileInfo> selected;
  selected.reserve(4);
  const auto add = [&](const Candidate* c, const uint8_t role) {
    if (!c) return;
    SdCardFontFileInfo info = files[c->index];
    info.style = role;
    selected.push_back(info);
  };
  add(regular, 0);
  add(bold, 1);
  add(italic, 2);
  add(boldItalic, 3);
  if (selected.size() < files.size()) {
    LOG_DBG("SDREG", "%s: %u of %u faces selected by weight", dirPath, static_cast<unsigned>(selected.size()),
            static_cast<unsigned>(files.size()));
  }
  files = std::move(selected);
}

#endif  // CROSSPOINT_VECTOR_FONTS

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  // Collected separately in one pass (the dir handle is forward-only), then whichever kind
  // the folder holds is committed below.
  std::vector<SdCardFontFileInfo> cpfontFiles;
  std::vector<SdCardFontFileInfo> vectorFiles;

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

    // Bounded copy, and SKIP rather than truncate: a truncated name builds a path
    // that either fails to open or names a different file.
    if (strlen(nameBuffer) >= SD_FONT_FILENAME_MAX) {
      LOG_ERR("SDREG", "Font name too long (%u >= %u), skipping: %s", (unsigned)strlen(nameBuffer),
              (unsigned)SD_FONT_FILENAME_MAX, nameBuffer);
      continue;
    }

    uint8_t size, style;
    if (parseFilename(nameBuffer, size, style)) {
      // Reject duplicate (pointSize, style) entries in the same family. With
      // v4's bundle-everything design parseFilename always returns style=0, so
      // two files at the same size in the same family would silently shadow
      // each other in findFile(). Skip the duplicate and warn.
      bool duplicate = false;
      for (const auto& existing : cpfontFiles) {
        if (existing.pointSize == size && existing.style == style) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", nameBuffer, dirPath);
        continue;
      }

      SdCardFontFileInfo info{};
      std::strcpy(info.filename, nameBuffer);
      info.pointSize = size;
      info.style = style;
      cpfontFiles.push_back(info);
      continue;
    }

#if CROSSPOINT_VECTOR_FONTS
    size_t baseLen = 0;
    if (parseVectorFontName(nameBuffer, baseLen)) {
      // Vector file in a family folder: seed the style role from the filename (e.g.
      // Merriweather/Merriweather-Italic.ttf -> italic); refineVectorStyles upgrades it from
      // the face's own metadata and dedups by role afterwards.
      SdCardFontFileInfo info{};
      std::strcpy(info.filename, nameBuffer);
      info.pointSize = 0;  // size-free
      info.style = parseVectorStyle(nameBuffer, baseLen);
      vectorFiles.push_back(info);
    }
#endif
  }

  // .cpfont wins if a folder somehow holds both: a pre-rasterized bitmap family is the more
  // specific artifact, and it costs no FreeType working set.
  if (!cpfontFiles.empty()) {
    family.vector = false;
    family.files = std::move(cpfontFiles);
    return;
  }
#if CROSSPOINT_VECTOR_FONTS
  if (!vectorFiles.empty()) {
    refineVectorStyles(dirPath, vectorFiles);
    family.vector = true;
    family.files = std::move(vectorFiles);
  }
#endif
}

// Discovery only needs to know whether a directory holds at least one admissible
// .cpfont (scanRoot's admission rule). Answering that without materialising the
// listing is what keeps the catalogue to a name and a root per family.
bool SdCardFontRegistry::hasAnyFontFile(const char* dirPath, bool* outVector) {
  if (outVector) *outVector = false;
  bool sawVector = false;
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
    // A .cpfont anywhere in the folder settles it immediately: it wins over vector files,
    // so there is nothing a later entry could change.
    if (parseFilename(nameBuffer, size, style)) return true;
#if CROSSPOINT_VECTOR_FONTS
    size_t baseLen = 0;
    if (parseVectorFontName(nameBuffer, baseLen)) {
      // Keep scanning: a .cpfont later in the folder still outranks this.
      if (outVector) *outVector = true;
      sawVector = true;
    }
#endif
  }
  return sawVector;
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
      bool isVector = false;
      if (!hasAnyFontFile(subDirPath, &isVector)) continue;

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      family.hiddenRoot = (strcmp(rootPath, FONTS_DIR_HIDDEN) == 0);
      // Retained on the catalogue: the font picker asks whether a family is size-free before
      // any listing is materialized.
      family.vector = isVector;
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
  // Seed from the catalogue so the flag is never stale if the rescan finds nothing;
  // scanDirectory overwrites it with what is actually on the card.
  fileCache_.vector = entry->vector;

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
