#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "BookmarkStore.h"
#include "CrossPointState.h"
#include "KOReaderDocumentId.h"
#include "LookupHistory.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/ReadingTimeHistory.h"

namespace {
constexpr char CACHE_BASE_DIR[] = "/.crosspoint";

// SD-log a relocation event regardless of the SdDebugLog gate: these are rare,
// one-shot events that must be inspectable untethered on the USB-locked X3 (no
// serial), and they happen outside the OPDS browser's enable window.
void sdLogReloc(const char* fmt, const char* a, const char* b) {
  const bool wasEnabled = SdDebugLog::isEnabled();
  SdDebugLog::setEnabled(true);
  SdDebugLog::log("RELOC", fmt, a, b);
  SdDebugLog::setEnabled(wasEnabled);
}

// content_id.bin: [0] version, [1..32] partial-MD5 hex of the book's content,
// [33..34] u16 LE path length, [35..] book path at the time the id was written.
// Written into each book's cache dir so a cache orphaned by an out-of-firmware
// move (PC/SD card) can be matched back to its book by content.
constexpr char CONTENT_ID_FILE[] = "/content_id.bin";
constexpr uint8_t CONTENT_ID_VERSION = 1;
constexpr size_t CONTENT_ID_MD5_LEN = 32;
constexpr uint16_t CONTENT_ID_MAX_PATH = 512;

// Cache dir prefix by book type; nullptr for non-book files.
const char* cacheDirPrefixForPath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return "epub_";
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return "xtc_";
  }
  if (FsHelpers::hasTxtExtension(path)) {
    return "txt_";
  }
  return nullptr;
}

// Mirrors the cache-key derivation in the Epub/Xtc/Txt constructors.
std::string cacheDirForPath(const char* prefix, const std::string& path) {
  return std::string(CACHE_BASE_DIR) + "/" + prefix + std::to_string(std::hash<std::string>{}(path));
}

// Renames the "<hash-dir>--<title>-by-<author>.txt" label file that sits next to an
// EPUB cache dir (see BookMetadataCache) so it keeps sorting adjacent to its renamed
// folder. Scans the listing first and renames after, so the directory handle is
// closed before the rename touches the same directory.
void relocateCacheLabel(const std::string& oldDirName, const std::string& newDirName) {
  const std::string oldPrefix = oldDirName + "--";
  std::string labelName;
  {
    HalFile dir = Storage.open(CACHE_BASE_DIR);
    if (!dir || !dir.isDirectory()) {
      return;
    }
    while (true) {
      HalFile f = dir.openNextFile();
      if (!f) {
        break;
      }
      char name[160];
      if (f.getName(name, sizeof(name)) == 0) {
        continue;
      }
      if (strncmp(name, oldPrefix.c_str(), oldPrefix.size()) == 0) {
        labelName = name;
        break;
      }
    }
  }
  if (labelName.empty()) {
    return;
  }
  const std::string oldLabel = std::string(CACHE_BASE_DIR) + "/" + labelName;
  const std::string newLabel = std::string(CACHE_BASE_DIR) + "/" + newDirName + labelName.substr(oldDirName.size());
  if (!Storage.rename(oldLabel.c_str(), newLabel.c_str())) {
    LOG_ERR("BookCache", "Failed to rename cache label %s (non-fatal)", oldLabel.c_str());
  }
}

// Deletes the "<dirName>--<title>-by-<author>.txt" SD-browsing label that sits next
// to a book cache dir, so it doesn't orphan in /.crosspoint once the cache dir is
// removed. Mirror of relocateCacheLabel's scan; closes the dir handle before remove.
void removeCacheLabel(const std::string& dirName) {
  const std::string prefix = dirName + "--";
  std::string labelName;
  {
    HalFile dir = Storage.open(CACHE_BASE_DIR);
    if (!dir || !dir.isDirectory()) {
      return;
    }
    while (true) {
      HalFile f = dir.openNextFile();
      if (!f) {
        break;
      }
      char name[160];
      if (f.getName(name, sizeof(name)) == 0) {
        continue;
      }
      if (strncmp(name, prefix.c_str(), prefix.size()) == 0) {
        labelName = name;
        break;
      }
    }
  }
  if (labelName.empty()) {
    return;
  }
  const std::string label = std::string(CACHE_BASE_DIR) + "/" + labelName;
  if (Storage.remove(label.c_str())) {
    LOG_DBG("BookCache", "Deleted cache label %s", label.c_str());
  } else {
    LOG_ERR("BookCache", "Failed to delete cache label %s (non-fatal)", label.c_str());
  }
}

bool writeContentId(const std::string& cacheDir, const std::string& md5Hex, const std::string& bookPath) {
  HalFile f;
  if (!Storage.openFileForWrite("BookCache", cacheDir + CONTENT_ID_FILE, f)) {
    return false;
  }
  const uint8_t version = CONTENT_ID_VERSION;
  const uint16_t pathLen = static_cast<uint16_t>(bookPath.size());
  bool ok = f.write(&version, 1) == 1;
  ok = ok && f.write(md5Hex.data(), CONTENT_ID_MD5_LEN) == CONTENT_ID_MD5_LEN;
  ok = ok && f.write(&pathLen, sizeof(pathLen)) == sizeof(pathLen);
  ok = ok && f.write(bookPath.data(), pathLen) == pathLen;
  if (!ok) {
    LOG_ERR("BookCache", "Failed to write %s%s", cacheDir.c_str(), CONTENT_ID_FILE);
  }
  return ok;
}

// Streaming byte copy through a fixed stack buffer — no heap. Used to seed small
// sidecar files at reader open, where the in-reader heap is starved.
bool copySmallFile(const std::string& src, const std::string& dst) {
  HalFile in;
  if (!Storage.openFileForRead("BookCache", src, in)) {
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("BookCache", dst, out)) {
    return false;
  }
  char buf[256];  // matches LookupHistory's fixed line-buffer convention
  int n;
  while ((n = in.read(buf, sizeof(buf))) > 0) {
    if (out.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      return false;
    }
  }
  return true;
}

bool readContentId(const std::string& cacheDir, std::string& outMd5, std::string& outPath) {
  const std::string idPath = cacheDir + CONTENT_ID_FILE;
  if (!Storage.exists(idPath.c_str())) {
    return false;  // pre-check avoids open-failure logging while scanning old cache dirs
  }
  HalFile f;
  if (!Storage.openFileForRead("BookCache", idPath, f)) {
    return false;
  }
  uint8_t version = 0;
  if (f.read(&version, 1) != 1 || version != CONTENT_ID_VERSION) {
    return false;
  }
  char md5[CONTENT_ID_MD5_LEN];
  if (f.read(md5, CONTENT_ID_MD5_LEN) != static_cast<int>(CONTENT_ID_MD5_LEN)) {
    return false;
  }
  uint16_t pathLen = 0;
  if (f.read(&pathLen, sizeof(pathLen)) != sizeof(pathLen) || pathLen == 0 || pathLen > CONTENT_ID_MAX_PATH) {
    return false;
  }
  outPath.resize(pathLen);
  if (f.read(outPath.data(), pathLen) != static_cast<int>(pathLen)) {
    return false;
  }
  outMd5.assign(md5, CONTENT_ID_MD5_LEN);
  return true;
}
}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  // clearCache() removes the path-hash cache dir but not the sibling
  // "<dir>--<title>.txt" SD-browsing label — remove it too so it doesn't orphan.
  // (Label is created for EPUBs; the scan is a cheap no-op for types without one.)
  if (const char* prefix = cacheDirPrefixForPath(path)) {
    removeCacheLabel(std::string(prefix) + std::to_string(std::hash<std::string>{}(path)));
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

void relocateBookBookmarks(const std::string& srcPath, const std::string& dstPath) {
  if (FsHelpers::hasEpubExtension(srcPath)) {
    BookmarkStore::relocateForFilePath(srcPath, dstPath, "epub");
  } else if (FsHelpers::hasXtcExtension(srcPath)) {
    BookmarkStore::relocateForFilePath(srcPath, dstPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(srcPath)) {
    BookmarkStore::relocateForFilePath(srcPath, dstPath, "txt");
  }
}

void relocateBookSidecars(const std::string& srcPath, const std::string& dstPath) {
  if (srcPath == dstPath) {
    return;
  }

  relocateBookBookmarks(srcPath, dstPath);

  const char* prefix = cacheDirPrefixForPath(srcPath);
  if (!prefix) {
    return;
  }

  const std::string oldDir = cacheDirForPath(prefix, srcPath);
  const std::string newDir = cacheDirForPath(prefix, dstPath);
  if (Storage.exists(oldDir.c_str())) {
    // A dir already at the new key can only be a stale orphan from a previous file at
    // dstPath (callers verify no file exists there before renaming) — replace it.
    if (Storage.exists(newDir.c_str())) {
      Storage.removeDir(newDir.c_str());
    }
    if (Storage.rename(oldDir.c_str(), newDir.c_str())) {
      LOG_DBG("BookCache", "Relocated cache dir %s -> %s", oldDir.c_str(), newDir.c_str());
      sdLogReloc("cache dir %s -> %s", oldDir.c_str(), newDir.c_str());
      if (strcmp(prefix, "epub_") == 0) {
        relocateCacheLabel(oldDir.substr(oldDir.rfind('/') + 1), newDir.substr(newDir.rfind('/') + 1));
      }
    } else {
      LOG_ERR("BookCache", "Failed to rename cache dir %s -> %s (non-fatal)", oldDir.c_str(), newDir.c_str());
      sdLogReloc("FAILED cache dir rename %s -> %s", oldDir.c_str(), newDir.c_str());
    }
  }

  // Keep the recents entry (and its cover thumb path) and the global resume pointer
  // pointing at the book's new location. Both are safe no-ops if they don't reference
  // srcPath.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldDir, newDir);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

void ensureCacheContentId(const std::string& bookPath, const std::string& cachePath) {
  const std::string idPath = cachePath + CONTENT_ID_FILE;
  if (Storage.exists(idPath.c_str())) {
    return;
  }
  const std::string md5 = KOReaderDocumentId::calculate(bookPath);
  if (md5.size() != CONTENT_ID_MD5_LEN) {
    return;
  }
  writeContentId(cachePath, md5, bookPath);
}

bool importSiblingStatsIfNew(const std::string& bookPath, const std::string& cachePath) {
  // Only device-tagged books have an untagged sibling to seed from. Try each
  // candidate (same-order, then author/title-swapped) and use the first present.
  std::string originPath;
  for (const auto& cand : siblingOriginPaths(bookPath)) {
    if (Storage.exists(cand.c_str())) {
      originPath = cand;
      break;
    }
  }
  if (originPath.empty()) {
    return false;
  }

  const char* prefix = cacheDirPrefixForPath(originPath);
  if (!prefix) {
    return false;
  }
  const std::string originCache = cacheDirForPath(prefix, originPath);

  bool imported = false;

  // Dated history (heatmap + timeline) — never synced, so this is the only way it
  // reaches the tagged copy. Copy only when the target has none yet (first cache
  // create) and the sibling actually has some, so we never clobber accrued data.
  const std::string dstHistory = cachePath + "/book_time_history.bin";
  const std::string srcHistory = originCache + "/book_time_history.bin";
  if (!Storage.exists(dstHistory.c_str()) && Storage.exists(srcHistory.c_str())) {
    auto history = makeUniqueNoThrow<ReadingTimeHistory>();
    if (history && ReadingTimeHistory::load(srcHistory, *history)) {
      ReadingTimeHistory::save(dstHistory, *history);
      LOG_INF("BookCache", "Imported reading history from sibling '%s'", originPath.c_str());
      imported = true;
    }
  }

  // Total seconds + last-read. KOReader sync self-heals these too, but copying
  // makes them correct immediately on first open, before any sync.
  const std::string srcStats = originCache + "/stats.bin";
  if (!Storage.exists((cachePath + "/stats.bin").c_str()) && Storage.exists(srcStats.c_str())) {
    BookReadingStats::load(originCache).save(cachePath);
    LOG_INF("BookCache", "Imported reading stats from sibling '%s'", originPath.c_str());
    imported = true;
  }

  // Dictionary lookup history — local-only, never synced. Seed once on first cache
  // create so a tagged/order-variant copy inherits the sibling's history. Byte copy,
  // not LookupHistory::load() (that materializes a vector in the heap-starved in-reader
  // context — see LookupHistory.h).
  const std::string dstDict = cachePath + "/" + LookupHistory::FILE_NAME;
  const std::string srcDict = originCache + "/" + LookupHistory::FILE_NAME;
  if (!Storage.exists(dstDict.c_str()) && Storage.exists(srcDict.c_str())) {
    if (copySmallFile(srcDict, dstDict)) {
      LOG_INF("BookCache", "Imported dictionary lookup history from sibling '%s'", originPath.c_str());
      imported = true;
    }
  }

  return imported;
}

bool tryRecoverBookCache(const std::string& bookPath) {
  const char* prefix = cacheDirPrefixForPath(bookPath);
  if (!prefix) {
    return false;
  }
  const std::string myDir = cacheDirForPath(prefix, bookPath);
  if (Storage.exists(myDir.c_str())) {
    return false;  // cache present under the current path; nothing to recover
  }

  const std::string myId = KOReaderDocumentId::calculate(bookPath);
  if (myId.size() != CONTENT_ID_MD5_LEN) {
    return false;
  }

  // Collect candidate dir names first so the directory handle is closed before any
  // rename touches the same directory.
  std::vector<std::string> candidates;
  candidates.reserve(16);
  {
    HalFile dir = Storage.open(CACHE_BASE_DIR);
    if (!dir || !dir.isDirectory()) {
      return false;
    }
    while (true) {
      HalFile f = dir.openNextFile();
      if (!f) {
        break;
      }
      if (!f.isDirectory()) {
        continue;
      }
      char name[160];
      if (f.getName(name, sizeof(name)) == 0) {
        continue;
      }
      if (strncmp(name, prefix, strlen(prefix)) == 0) {
        candidates.emplace_back(name);
      }
    }
  }

  for (const auto& dirName : candidates) {
    const std::string dirPath = std::string(CACHE_BASE_DIR) + "/" + dirName;
    std::string id;
    std::string oldPath;
    if (!readContentId(dirPath, id, oldPath)) {
      continue;
    }
    if (id != myId) {
      continue;
    }
    if (Storage.exists(oldPath.c_str())) {
      continue;  // original file still present — a duplicate copy, not a move
    }
    if (cacheDirForPath(prefix, oldPath) != dirPath) {
      continue;  // stale/corrupt id record; dir was not created for that path
    }
    LOG_INF("BookCache", "Recovering cache for moved book: %s -> %s", oldPath.c_str(), bookPath.c_str());
    sdLogReloc("recovering moved book %s -> %s", oldPath.c_str(), bookPath.c_str());
    relocateBookSidecars(oldPath, bookPath);
    if (Storage.exists(myDir.c_str())) {
      writeContentId(myDir, myId, bookPath);  // refresh the recorded path
      return true;
    }
    return false;  // relocation failed; logged inside relocateBookSidecars
  }
  return false;
}

CachePruneResult scanOrphanCaches(std::vector<std::string>& orphanDirNames) {
  CachePruneResult res;

  // Snapshot cache dir names with the listing handle open, act after it closes —
  // mirrors tryRecoverBookCache() (a later removeDir must not run against an open
  // directory handle).
  std::vector<std::string> candidates;
  candidates.reserve(16);
  {
    HalFile dir = Storage.open(CACHE_BASE_DIR);
    if (!dir || !dir.isDirectory()) {
      return res;
    }
    while (true) {
      HalFile f = dir.openNextFile();
      if (!f) {
        break;
      }
      if (!f.isDirectory()) {
        continue;
      }
      char name[160];
      if (f.getName(name, sizeof(name)) == 0) {
        continue;
      }
      if (isBookCacheDirectoryName(name)) {
        candidates.emplace_back(name);
      }
    }
  }

  for (const auto& dirName : candidates) {
    const std::string dirPath = std::string(CACHE_BASE_DIR) + "/" + dirName;
    std::string id;
    std::string recordedPath;
    if (!readContentId(dirPath, id, recordedPath)) {
      res.skipped++;  // no/unreadable fingerprint (legacy) — cannot prove orphan, leave it
      continue;
    }
    const char* prefix = cacheDirPrefixForPath(recordedPath);
    if (!prefix || cacheDirForPath(prefix, recordedPath) != dirPath) {
      res.skipped++;  // corrupt/foreign id record — dir was not created for that path
      continue;
    }
    if (Storage.exists(recordedPath.c_str())) {
      res.kept++;  // book still on the card under its recorded path
      continue;
    }
    // Orphan: recorded book is gone (deleted, or moved out-of-firmware and never
    // reopened). Record it for the caller to preview/remove.
    orphanDirNames.push_back(dirName);
    res.removed++;  // "found" count; not yet removed
  }
  return res;
}

CachePruneResult removeOrphanCaches(const std::vector<std::string>& orphanDirNames) {
  CachePruneResult res;
  for (const auto& dirName : orphanDirNames) {
    const std::string dirPath = std::string(CACHE_BASE_DIR) + "/" + dirName;
    if (Storage.removeDir(dirPath.c_str())) {
      removeCacheLabel(dirName);
      LOG_INF("BookCache", "Pruned orphan cache %s", dirName.c_str());
      res.removed++;
    } else {
      LOG_ERR("BookCache", "Failed to prune orphan cache %s (non-fatal)", dirPath.c_str());
      res.failed++;
    }
  }
  return res;
}

CachePruneResult pruneOrphanCaches() {
  std::vector<std::string> orphans;
  orphans.reserve(16);
  CachePruneResult res = scanOrphanCaches(orphans);
  const CachePruneResult rm = removeOrphanCaches(orphans);
  res.removed = rm.removed;  // scan set removed=found; replace with actually-removed
  res.failed = rm.failed;
  return res;
}
