#include "BookmarkStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr uint8_t LEGACY_VERSION = 2;
constexpr uint8_t COUNT_U16_VERSION = 3;
constexpr uint8_t PARAGRAPH_ANCHOR_VERSION = 4;
constexpr uint8_t SNIPPET_VERSION = 5;
constexpr uint8_t RETURN_MARK_VERSION = 6;  // adds a per-bookmark "return here" flag byte
constexpr uint8_t CHAPTER_PAGES_VERSION = 7;  // adds chapterCurrentPage + chapterPageCount (two uint16)
constexpr uint8_t VERSION = 7;
constexpr bool isKnownVersion(uint8_t v) {
  return v == LEGACY_VERSION || v == COUNT_U16_VERSION || v == PARAGRAPH_ANCHOR_VERSION || v == SNIPPET_VERSION ||
         v == RETURN_MARK_VERSION || v == CHAPTER_PAGES_VERSION;
}
// Stored count is uint16_t in v3+, but we keep an in-memory safety cap for ESP32-C3 RAM.
constexpr uint16_t MAX_BOOKMARKS = 1024;
constexpr size_t INITIAL_BOOKMARK_RESERVE = 8;
constexpr char BOOKMARKS_DIR[] = "/.crosspoint/bookmarks";

bool readBookmarkCount(HalFile& file, const uint8_t version, uint16_t& count) {
  if (version == LEGACY_VERSION) {
    uint8_t legacyCount = 0;
    serialization::readPod(file, legacyCount);
    count = legacyCount;
    return true;
  }

  if (version >= COUNT_U16_VERSION) {
    serialization::readPod(file, count);
    return true;
  }

  return false;
}

// Two entries are "the same" when they share a spine and either the same paragraph anchor
// (stable across render settings) or, when no anchor exists, the same quantized intra-spine
// progress. Quantizing avoids float-equality misses when the value round-trips through JSON.
constexpr float PROGRESS_QUANTUM = 1000.0f;
constexpr uint8_t TOMB_LEGACY_VERSION = 1;  // no per-tombstone Lamport version (reads back as 0)
constexpr uint8_t TOMB_VERSION = 2;         // adds uint32_t version per tombstone

bool keyMatch(uint16_t aSpine, uint16_t aPara, float aProg, uint16_t bSpine, uint16_t bPara, float bProg) {
  if (aSpine != bSpine) return false;
  const bool aHasAnchor = aPara != UINT16_MAX;
  const bool bHasAnchor = bPara != UINT16_MAX;
  if (aHasAnchor && bHasAnchor) return aPara == bPara;
  return std::lround(aProg * PROGRESS_QUANTUM) == std::lround(bProg * PROGRESS_QUANTUM);
}

bool sameBookmark(const Bookmark& a, const Bookmark& b) {
  return keyMatch(a.spineIndex, a.paragraphIndex, a.progress, b.spineIndex, b.paragraphIndex, b.progress);
}
bool sameTomb(const Tombstone& a, const Tombstone& b) {
  return keyMatch(a.spineIndex, a.paragraphIndex, a.progress, b.spineIndex, b.paragraphIndex, b.progress);
}
bool tombHits(const Tombstone& t, const Bookmark& b) {
  return keyMatch(t.spineIndex, t.paragraphIndex, t.progress, b.spineIndex, b.paragraphIndex, b.progress);
}
}  // namespace

BookmarkStore BookmarkStore::instance;

bool BookmarkStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub" && bookType != "xtc" && bookType != "txt") {
    LOG_ERR("BKS", "Unknown book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  lamportCounter = 0;  // reset the per-book clock; rebuilt from file versions below
  bookmarks.clear();
  if (bookmarks.capacity() < INITIAL_BOOKMARK_RESERVE) {
    bookmarks.reserve(INITIAL_BOOKMARK_RESERVE);
  }

  const uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(filePath.data()), static_cast<uint32_t>(filePath.size()));
  storeFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";

  // Tombstones live in a parallel file so the bookmark format stays untouched.
  tombstones.clear();
  tombDirty = false;
  tombFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".tomb";
  if (Storage.exists(tombFilePath.c_str())) readTombstones();

  if (!Storage.exists(storeFilePath.c_str())) {
    LOG_DBG("BKS", "No bookmark file for this book");
    return true;
  }

  return readFromFile();
}

void BookmarkStore::unload() {
  if (dirty) saveToFile();
  if (tombDirty) saveTombstones();
  bookmarks.clear();
  tombstones.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  tombFilePath.clear();
  dirty = false;
  tombDirty = false;
}

BookmarkStore::AddResult BookmarkStore::addBookmark(uint16_t spineIndex, float progress, int pageCount,
                                                    const char* chapterTitle, uint16_t paragraphIndex,
                                                    const char* snippet, bool returnMark, int currentPage) {
  if (pageCount > 0) {
    const float pageSlice = 1.0f / static_cast<float>(pageCount);
    const float pageStart = progress;
    const float pageEnd = progress + pageSlice;
    std::erase_if(bookmarks, [&](const Bookmark& b) {
      return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
    });
  }

  if (bookmarks.size() >= MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark limit (%u) reached", MAX_BOOKMARKS);
    return AddResult::LimitReached;
  }

  Bookmark bm{};
  bm.spineIndex = spineIndex;
  bm.progress = progress;
  bm.version = nextVersion();  // newer than any prior add/delete for this book
  snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", chapterTitle ? chapterTitle : "");
  bm.paragraphIndex = paragraphIndex;
  snprintf(bm.snippet, sizeof(bm.snippet), "%s", snippet ? snippet : "");
  bm.returnMark = returnMark;
  bm.chapterCurrentPage = static_cast<uint16_t>(currentPage < 0 ? 0 : currentPage);
  bm.chapterPageCount = static_cast<uint16_t>(pageCount < 0 ? 0 : pageCount);

  bookmarks.push_back(bm);
  sortBookmarks();
  clearTombstoneFor(bm);  // re-bookmarking a deleted spot un-deletes it
  dirty = true;
  saveToFile();
  return AddResult::Added;
}

void BookmarkStore::removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
  if (it == bookmarks.end()) return;

  if (!it->returnMark) addTombstone(*it);  // device-only return marks never synced, so no tombstone
  bookmarks.erase(it);
  dirty = true;
  saveToFile();
}

bool BookmarkStore::removeBookmarkAt(size_t index) {
  if (index >= bookmarks.size()) return false;

  if (!bookmarks[index].returnMark) addTombstone(bookmarks[index]);  // device-only: no tombstone for return marks
  bookmarks.erase(bookmarks.begin() + index);
  dirty = true;
  saveToFile();
  return true;
}

bool BookmarkStore::removeReturnMarkAt(uint16_t spineIndex, uint16_t paragraphIndex, float progress) {
  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.returnMark && keyMatch(b.spineIndex, b.paragraphIndex, b.progress, spineIndex, paragraphIndex, progress);
  });
  if (it == bookmarks.end()) return false;

  // Device-only mark: it was never pushed, so no tombstone is needed to propagate a delete.
  bookmarks.erase(it);
  dirty = true;
  saveToFile();
  return true;
}

bool BookmarkStore::hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
}

bool BookmarkStore::isReturnMarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  const float pageSlice = 1.0f / static_cast<float>(pageCount);
  const float pageStart = pageProgress;
  const float pageEnd = pageProgress + pageSlice;

  return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.returnMark && b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
}

void BookmarkStore::saveToFile() {
  if (!dirty || storeFilePath.empty()) return;
  if (bookmarks.empty()) {
    if (Storage.exists(storeFilePath.c_str())) Storage.remove(storeFilePath.c_str());
    dirty = false;
    return;
  }
  if (writeToFile()) dirty = false;
}

void BookmarkStore::clearAll() {
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    if (!Storage.remove(storeFilePath.c_str())) {
      LOG_ERR("BKS", "Failed to delete bookmark file");
      return;
    }
    LOG_DBG("BKS", "Bookmark file deleted");
  }
  // Tombstone every cleared bookmark so the wipe propagates on next sync. Each gets
  // a fresh Lamport version (newer than the bookmark) so the delete wins the merge.
  for (const auto& bm : bookmarks) {
    if (bm.returnMark) continue;  // device-only return marks were never synced — nothing to propagate
    const uint32_t v = nextVersion();
    auto it = std::find_if(tombstones.begin(), tombstones.end(),
                           [&](const Tombstone& e) { return sameTomb(e, Tombstone{bm.spineIndex, bm.paragraphIndex, bm.progress, 0}); });
    if (it != tombstones.end()) {
      it->version = v;
    } else {
      tombstones.push_back(Tombstone{bm.spineIndex, bm.paragraphIndex, bm.progress, v});
    }
    tombDirty = true;
  }
  saveTombstones();
  bookmarks.clear();
  dirty = false;
}

bool BookmarkStore::readFromFile() {
  HalFile f;
  if (!Storage.openFileForRead("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for read");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (!isKnownVersion(version)) {
    LOG_ERR("BKS", "Unknown bookmark file version: %u", version);
    return false;
  }

  uint16_t count = 0;
  if (!readBookmarkCount(f, version, count)) {
    LOG_ERR("BKS", "Failed to read bookmark count for version %u", version);
    return false;
  }
  if (count > MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark count %u exceeds max, file may be corrupt", count);
    return false;
  }

  std::string tmp;
  serialization::readString(f, tmp);  // title — not validated
  serialization::readString(f, tmp);  // author — not validated
  std::string storedPath;
  serialization::readString(f, storedPath);
  if (storedPath != bookFilePath) {
    LOG_ERR("BKS", "Bookmark file path mismatch, file may belong to a different book");
    return false;
  }

  bookmarks.clear();
  bookmarks.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    Bookmark bm{};
    if (f.available() < static_cast<int>(sizeof(bm.spineIndex))) {
      LOG_ERR("BKS", "Bookmark file truncated at spineIndex, record %u", i);
      return false;
    }
    serialization::readPod(f, bm.spineIndex);
    if (f.available() < static_cast<int>(sizeof(bm.progress))) {
      LOG_ERR("BKS", "Bookmark file truncated at progress, record %u", i);
      return false;
    }
    serialization::readPod(f, bm.progress);
    if (f.available() < static_cast<int>(sizeof(bm.version))) {
      LOG_ERR("BKS", "Bookmark file truncated at version, record %u", i);
      return false;
    }
    serialization::readPod(f, bm.version);  // legacy files stored 0 here (old "timestamp")
    const int chRead = f.read(bm.chapterTitle, sizeof(bm.chapterTitle));
    bm.chapterTitle[sizeof(bm.chapterTitle) - 1] = '\0';
    if (chRead != static_cast<int>(sizeof(bm.chapterTitle))) {
      LOG_ERR("BKS", "Bookmark file truncated at chapterTitle, record %u", i);
      return false;
    }
    if (version >= PARAGRAPH_ANCHOR_VERSION) {
      if (f.available() < static_cast<int>(sizeof(bm.paragraphIndex))) {
        LOG_ERR("BKS", "Bookmark file truncated at paragraphIndex, record %u", i);
        return false;
      }
      serialization::readPod(f, bm.paragraphIndex);
    } else {
      bm.paragraphIndex = UINT16_MAX;
    }
    if (version >= SNIPPET_VERSION) {
      const int snippetRead = f.read(bm.snippet, sizeof(bm.snippet));
      bm.snippet[sizeof(bm.snippet) - 1] = '\0';
      if (snippetRead != static_cast<int>(sizeof(bm.snippet))) {
        LOG_ERR("BKS", "Bookmark file truncated at snippet, record %u", i);
        return false;
      }
    } else {
      bm.snippet[0] = '\0';
    }
    if (version >= RETURN_MARK_VERSION) {
      uint8_t returnFlag = 0;
      if (f.available() < static_cast<int>(sizeof(returnFlag))) {
        LOG_ERR("BKS", "Bookmark file truncated at returnMark, record %u", i);
        return false;
      }
      serialization::readPod(f, returnFlag);
      bm.returnMark = returnFlag != 0;
    } else {
      bm.returnMark = false;
    }
    if (version >= CHAPTER_PAGES_VERSION) {
      if (f.available() < static_cast<int>(sizeof(bm.chapterCurrentPage) + sizeof(bm.chapterPageCount))) {
        LOG_ERR("BKS", "Bookmark file truncated at chapter pages, record %u", i);
        return false;
      }
      serialization::readPod(f, bm.chapterCurrentPage);
      serialization::readPod(f, bm.chapterPageCount);
    } else {
      bm.chapterCurrentPage = 0;
      bm.chapterPageCount = 0;  // legacy bookmark: page position unknown, list shows title only
    }
    observeVersion(bm.version);  // rebuild the Lamport clock from stored versions
    bookmarks.push_back(bm);
  }

  sortBookmarks();  // present the list ordered by section then position

  if (version != VERSION) {
    dirty = true;
    saveToFile();
    LOG_DBG("BKS", "Migrated bookmark file to version %u", VERSION);
  }
  LOG_DBG("BKS", "Loaded %u bookmark(s)", count);
  return true;
}

bool BookmarkStore::writeToFile() const {
  Storage.mkdir(BOOKMARKS_DIR);

  HalFile f;
  if (!Storage.openFileForWrite("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for write");
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(bookmarks.size());
  serialization::writePod(f, VERSION);
  serialization::writePod(f, count);
  serialization::writeString(f, bookTitle);
  serialization::writeString(f, bookAuthor);
  serialization::writeString(f, bookFilePath);

  for (const auto& bm : bookmarks) {
    serialization::writePod(f, bm.spineIndex);
    serialization::writePod(f, bm.progress);
    serialization::writePod(f, bm.version);
    f.write(bm.chapterTitle, sizeof(bm.chapterTitle));
    serialization::writePod(f, bm.paragraphIndex);
    f.write(bm.snippet, sizeof(bm.snippet));
    const uint8_t returnFlag = bm.returnMark ? 1 : 0;
    serialization::writePod(f, returnFlag);
    serialization::writePod(f, bm.chapterCurrentPage);
    serialization::writePod(f, bm.chapterPageCount);
  }

  LOG_DBG("BKS", "Saved %u bookmark(s)", count);
  return true;
}

std::string BookmarkStore::serializeToJson(const std::vector<Bookmark>& bms, const std::vector<Tombstone>& tombs) {
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  for (const auto& bm : bms) {
    if (bm.returnMark) continue;  // session "return here" mark is device-only — never synced
    JsonObject obj = arr.add<JsonObject>();
    obj["spineIndex"] = bm.spineIndex;
    obj["progress"] = bm.progress;
    obj["version"] = bm.version;  // Lamport version (formerly the always-0 "timestamp")
    obj["chapterTitle"] = bm.chapterTitle;
    obj["paragraphIndex"] = bm.paragraphIndex;
    obj["snippet"] = bm.snippet;
    obj["chapterCurrentPage"] = bm.chapterCurrentPage;
    obj["chapterPageCount"] = bm.chapterPageCount;
  }
  JsonArray tarr = doc["tombstones"].to<JsonArray>();
  for (const auto& t : tombs) {
    JsonObject obj = tarr.add<JsonObject>();
    obj["spineIndex"] = t.spineIndex;
    obj["paragraphIndex"] = t.paragraphIndex;
    obj["progress"] = t.progress;
    obj["version"] = t.version;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool BookmarkStore::parseFromJson(const char* json, std::vector<Bookmark>& outBms, std::vector<Tombstone>& outTombs) {
  outBms.clear();
  outTombs.clear();
  if (json == nullptr || json[0] == '\0') return true;  // empty blob = nothing stored

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("BKS", "Bookmark sync JSON parse error: %s", err.c_str());
    return false;
  }

  // Legacy bare-array form is just the bookmark list; new form nests it under "bookmarks".
  JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["bookmarks"].as<JsonArray>();
  outBms.reserve(arr.size());
  for (JsonObject obj : arr) {
    if (outBms.size() >= MAX_BOOKMARKS) break;
    Bookmark bm{};
    bm.spineIndex = obj["spineIndex"] | static_cast<uint16_t>(0);
    bm.progress = obj["progress"] | 0.0f;
    // New key is "version"; fall back to the legacy "timestamp" key (older blobs).
    bm.version = obj["version"] | (obj["timestamp"] | static_cast<uint32_t>(0));
    bm.paragraphIndex = obj["paragraphIndex"] | static_cast<uint16_t>(UINT16_MAX);
    snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", obj["chapterTitle"] | "");
    snprintf(bm.snippet, sizeof(bm.snippet), "%s", obj["snippet"] | "");
    // Display-only page snapshot; absent from older/other-firmware blobs → 0 (unknown).
    bm.chapterCurrentPage = obj["chapterCurrentPage"] | static_cast<uint16_t>(0);
    bm.chapterPageCount = obj["chapterPageCount"] | static_cast<uint16_t>(0);
    outBms.push_back(bm);
  }

  JsonArray tarr = doc["tombstones"].as<JsonArray>();
  outTombs.reserve(tarr.size());
  for (JsonObject obj : tarr) {
    if (outTombs.size() >= MAX_BOOKMARKS) break;
    Tombstone t{};
    t.spineIndex = obj["spineIndex"] | static_cast<uint16_t>(0);
    t.paragraphIndex = obj["paragraphIndex"] | static_cast<uint16_t>(UINT16_MAX);
    t.progress = obj["progress"] | 0.0f;
    t.version = obj["version"] | static_cast<uint32_t>(0);
    outTombs.push_back(t);
  }
  return true;
}

void BookmarkStore::sortBookmarks() {
  std::sort(bookmarks.begin(), bookmarks.end(), [](const Bookmark& a, const Bookmark& b) {
    if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
    if (a.progress != b.progress) return a.progress < b.progress;
    return a.paragraphIndex < b.paragraphIndex;
  });
}

bool BookmarkStore::readTombstones() {
  HalFile f;
  if (!Storage.openFileForRead("BKS", tombFilePath, f)) return false;

  uint8_t version;
  serialization::readPod(f, version);
  if (version != TOMB_LEGACY_VERSION && version != TOMB_VERSION) {
    LOG_ERR("BKS", "Unknown tombstone file version: %u", version);
    return false;
  }
  if (f.available() < static_cast<int>(sizeof(uint16_t))) return false;
  uint16_t count = 0;
  serialization::readPod(f, count);
  if (count > MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Tombstone count %u exceeds max, file may be corrupt", count);
    return false;
  }

  tombstones.clear();
  tombstones.reserve(count);
  int recordSize = static_cast<int>(sizeof(uint16_t) + sizeof(uint16_t) + sizeof(float));
  if (version >= TOMB_VERSION) recordSize += static_cast<int>(sizeof(uint32_t));
  for (uint16_t i = 0; i < count; i++) {
    if (f.available() < recordSize) {
      LOG_ERR("BKS", "Tombstone file truncated at record %u", i);
      return false;
    }
    Tombstone t{};
    serialization::readPod(f, t.spineIndex);
    serialization::readPod(f, t.paragraphIndex);
    serialization::readPod(f, t.progress);
    if (version >= TOMB_VERSION) {
      serialization::readPod(f, t.version);
    } else {
      t.version = 0;  // legacy tombstone: lowest priority, loses to any stamped edit
    }
    observeVersion(t.version);
    tombstones.push_back(t);
  }

  if (version != TOMB_VERSION) {
    tombDirty = true;
    saveTombstones();  // migrate legacy .tomb to the versioned format
    LOG_DBG("BKS", "Migrated tombstone file to version %u", TOMB_VERSION);
  }
  LOG_DBG("BKS", "Loaded %u tombstone(s)", count);
  return true;
}

bool BookmarkStore::writeTombstones() const {
  Storage.mkdir(BOOKMARKS_DIR);

  HalFile f;
  if (!Storage.openFileForWrite("BKS", tombFilePath, f)) {
    LOG_ERR("BKS", "Failed to open tombstone file for write");
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(tombstones.size());
  serialization::writePod(f, TOMB_VERSION);
  serialization::writePod(f, count);
  for (const auto& t : tombstones) {
    serialization::writePod(f, t.spineIndex);
    serialization::writePod(f, t.paragraphIndex);
    serialization::writePod(f, t.progress);
    serialization::writePod(f, t.version);
  }
  LOG_DBG("BKS", "Saved %u tombstone(s)", count);
  return true;
}

void BookmarkStore::saveTombstones() {
  if (!tombDirty || tombFilePath.empty()) return;
  if (tombstones.empty()) {
    if (Storage.exists(tombFilePath.c_str())) Storage.remove(tombFilePath.c_str());
    tombDirty = false;
    return;
  }
  if (writeTombstones()) tombDirty = false;
}

void BookmarkStore::addTombstone(const Bookmark& bm) {
  // Stamp newer than the bookmark being deleted so the delete wins the merge.
  const uint32_t v = nextVersion();
  for (auto& e : tombstones) {
    if (sameTomb(e, Tombstone{bm.spineIndex, bm.paragraphIndex, bm.progress, 0})) {
      e.version = v;  // refresh an existing tombstone so a re-delete still outranks
      tombDirty = true;
      saveTombstones();
      return;
    }
  }
  tombstones.push_back(Tombstone{bm.spineIndex, bm.paragraphIndex, bm.progress, v});
  tombDirty = true;
  saveTombstones();
}

void BookmarkStore::clearTombstoneFor(const Bookmark& bm) {
  const size_t before = tombstones.size();
  std::erase_if(tombstones, [&](const Tombstone& t) { return tombHits(t, bm); });
  if (tombstones.size() != before) {
    tombDirty = true;
    saveTombstones();
  }
}

size_t BookmarkStore::mergeFrom(const std::vector<Bookmark>& remoteBookmarks,
                                const std::vector<Tombstone>& remoteTombstones) {
  // Last-writer-wins by Lamport version. For each spot, keep whichever of
  // {bookmark, tombstone} carries the highest version across local + remote: a
  // delete done after seeing a bookmark (higher version) propagates, and a re-add
  // done after a delete (higher still) resurrects — both converge with no clock.
  // On a version tie the live bookmark wins (never silently drop a user bookmark).
  // Winners reference the source records by pointer to avoid copying Bookmark
  // structs (~124 B each) during the merge.
  struct Winner {
    uint16_t spineIndex;
    uint16_t paragraphIndex;
    float progress;
    uint32_t version;
    const Bookmark* bm;  // non-null => bookmark wins this spot; null => tombstone wins
  };
  std::vector<Winner> winners;
  winners.reserve(bookmarks.size() + tombstones.size() + remoteBookmarks.size() + remoteTombstones.size());

  const auto consider = [&](uint16_t spine, uint16_t para, float prog, uint32_t version, const Bookmark* bm) {
    observeVersion(version);  // advance the local clock past everything we see
    for (auto& w : winners) {
      if (keyMatch(w.spineIndex, w.paragraphIndex, w.progress, spine, para, prog)) {
        const bool better = version > w.version || (version == w.version && bm != nullptr && w.bm == nullptr);
        if (better) {
          w.version = version;
          w.bm = bm;
        }
        return;
      }
    }
    winners.push_back(Winner{spine, para, prog, version, bm});
  };

  for (const auto& b : bookmarks) consider(b.spineIndex, b.paragraphIndex, b.progress, b.version, &b);
  for (const auto& t : tombstones) consider(t.spineIndex, t.paragraphIndex, t.progress, t.version, nullptr);
  for (const auto& b : remoteBookmarks) consider(b.spineIndex, b.paragraphIndex, b.progress, b.version, &b);
  for (const auto& t : remoteTombstones) consider(t.spineIndex, t.paragraphIndex, t.progress, t.version, nullptr);

  // Rebuild the bookmark + tombstone sets from the winners. Build into fresh
  // vectors first; pointers in `winners` may reference the current `bookmarks`,
  // so don't mutate it until the copies are made.
  std::vector<Bookmark> newBookmarks;
  std::vector<Tombstone> newTombstones;
  newBookmarks.reserve(winners.size());
  newTombstones.reserve(winners.size());
  for (const auto& w : winners) {
    if (w.bm != nullptr) {
      if (newBookmarks.size() >= MAX_BOOKMARKS) {
        LOG_ERR("BKS", "Bookmark limit (%u) reached during merge", MAX_BOOKMARKS);
        continue;
      }
      newBookmarks.push_back(*w.bm);
    } else {
      newTombstones.push_back(Tombstone{w.spineIndex, w.paragraphIndex, w.progress, w.version});
    }
  }

  // Count bookmarks now present that weren't local before (for the return/log).
  size_t added = 0;
  for (const auto& nb : newBookmarks) {
    const bool wasLocal = std::any_of(bookmarks.begin(), bookmarks.end(),
                                      [&](const Bookmark& l) { return sameBookmark(l, nb); });
    if (!wasLocal) ++added;
  }

  // Persist only when something actually changed (sync is rare, but skip needless SD writes).
  const auto bookmarkSetEqual = [](const std::vector<Bookmark>& a, const std::vector<Bookmark>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& x : a) {
      const bool match = std::any_of(b.begin(), b.end(),
                                     [&](const Bookmark& y) { return sameBookmark(x, y) && x.version == y.version; });
      if (!match) return false;
    }
    return true;
  };
  const auto tombSetEqual = [](const std::vector<Tombstone>& a, const std::vector<Tombstone>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& x : a) {
      const bool match =
          std::any_of(b.begin(), b.end(), [&](const Tombstone& y) { return sameTomb(x, y) && x.version == y.version; });
      if (!match) return false;
    }
    return true;
  };

  const bool bookmarksChanged = !bookmarkSetEqual(newBookmarks, bookmarks);
  const bool tombsChanged = !tombSetEqual(newTombstones, tombstones);

  bookmarks.swap(newBookmarks);
  tombstones.swap(newTombstones);

  if (bookmarksChanged) {
    dirty = true;
    sortBookmarks();
    saveToFile();
  }
  if (tombsChanged) {
    tombDirty = true;
    saveTombstones();
  }
  return added;
}

void BookmarkStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(filePath.data()), static_cast<uint32_t>(filePath.size()));
  const std::string base = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc);
  // Remove both the bookmark file and its tombstone sidecar.
  for (const std::string& path : {base + ".bin", base + ".tomb"}) {
    if (!Storage.exists(path.c_str())) continue;
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("BKS", "Failed to delete file: %s", path.c_str());
    } else {
      LOG_DBG("BKS", "Deleted %s", path.c_str());
    }
  }
}

bool BookmarkStore::hasAnyBookmarks() {
  if (!Storage.exists(BOOKMARKS_DIR)) return false;
  // A lone .tomb (book whose bookmarks were all deleted) does not count as having bookmarks.
  const auto files = Storage.listFiles(BOOKMARKS_DIR);
  return std::any_of(files.begin(), files.end(),
                     [](const auto& name) { return std::string(name.c_str()).find(".tomb") == std::string::npos; });
}

bool BookmarkStore::getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out) {
  if (!Storage.exists(BOOKMARKS_DIR)) return true;

  const auto files = Storage.listFiles(BOOKMARKS_DIR);
  for (const auto& name : files) {
    const std::string fullPath = std::string(BOOKMARKS_DIR) + "/" + name.c_str();

    HalFile f;
    if (!Storage.openFileForRead("BKS", fullPath, f)) continue;

    if (f.available() < static_cast<int>(sizeof(uint8_t))) {
      continue;
    }
    uint8_t version;
    serialization::readPod(f, version);
    if (!isKnownVersion(version)) {
      LOG_DBG("BKS", "Skipping bookmark file with unknown version: %s", name.c_str());
      continue;
    }

    if (f.available() < static_cast<int>(version == LEGACY_VERSION ? sizeof(uint8_t) : sizeof(uint16_t))) {
      continue;
    }
    uint16_t count = 0;
    if (!readBookmarkCount(f, version, count)) {
      continue;
    }

    auto readCheckedString = [&f](std::string& s) -> bool {
      uint32_t len;
      if (f.available() < static_cast<int>(sizeof(len))) return false;
      serialization::readPod(f, len);
      if (f.available() < static_cast<int>(len)) return false;
      s.resize(len);
      f.read(&s[0], len);
      return true;
    };

    std::string title, author, path;
    if (!readCheckedString(title) || !readCheckedString(author) || !readCheckedString(path)) {
      continue;
    }

    if (path.empty() || count == 0) continue;
    if (!Storage.exists(path.c_str())) continue;

    std::string bookType = "epub";
    const std::string nameStr = name.c_str();
    size_t underscorePos = nameStr.find('_');
    if (underscorePos != std::string::npos) {
      bookType = nameStr.substr(0, underscorePos);
    }

    out.push_back({std::move(title), std::move(author), std::move(path), std::move(bookType), count});
  }

  return true;
}
