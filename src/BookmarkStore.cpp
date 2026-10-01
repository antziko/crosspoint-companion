#include "BookmarkStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <Serialization.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr uint8_t LEGACY_VERSION = 2;
constexpr uint8_t COUNT_U16_VERSION = 3;
constexpr uint8_t PARAGRAPH_ANCHOR_VERSION = 4;
constexpr uint8_t SNIPPET_VERSION = 5;
constexpr uint8_t RETURN_MARK_VERSION = 6;     // adds a per-bookmark "return here" flag byte
constexpr uint8_t CHAPTER_PAGES_VERSION = 7;   // adds chapterCurrentPage + chapterPageCount (two uint16)
constexpr uint8_t QUOTE_RANGE_VERSION = 8;     // adds quote flag + end anchor + start/end word (ranged quotes)
constexpr uint8_t VISIBLE_OFFSET_VERSION = 9;  // adds visibleTextOffset (uint32), 0 when unknown
constexpr uint8_t VERSION = 9;
constexpr bool isKnownVersion(uint8_t v) {
  return v == LEGACY_VERSION || v == COUNT_U16_VERSION || v == PARAGRAPH_ANCHOR_VERSION || v == SNIPPET_VERSION ||
         v == RETURN_MARK_VERSION || v == CHAPTER_PAGES_VERSION || v == QUOTE_RANGE_VERSION ||
         v == VISIBLE_OFFSET_VERSION;
}
constexpr size_t INITIAL_BOOKMARK_RESERVE = 8;
constexpr char BOOKMARKS_DIR[] = "/.crosspoint/bookmarks";
// Parallel full-preview store; one append-only, identity-keyed entry per quote.
constexpr uint8_t QTEXT_VERSION = 1;
// Parallel KOReader-XPath store; one append-only, identity-keyed entry per bookmark.
constexpr uint8_t XPATH_VERSION = 2;
constexpr char HIGHLIGHTS_DIR[] = "/highlights";

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
constexpr uint8_t TOMB_VERSION_V2 = 2;      // adds uint32_t version per tombstone
constexpr uint8_t TOMB_VERSION = 3;         // adds quote flag + start/end word (ranged quote identity)

// Point-bookmark identity: same spine and either the same paragraph anchor (stable across
// render settings) or, when no anchor exists, the same quantized intra-spine progress.
bool pointKeyMatch(uint16_t aSpine, uint16_t aPara, float aProg, uint16_t bSpine, uint16_t bPara, float bProg) {
  if (aSpine != bSpine) return false;
  const bool aHasAnchor = aPara != UINT16_MAX;
  const bool bHasAnchor = bPara != UINT16_MAX;
  if (aHasAnchor && bHasAnchor) return aPara == bPara;
  return std::lround(aProg * PROGRESS_QUANTUM) == std::lround(bProg * PROGRESS_QUANTUM);
}

// Unified identity. A quote and a point bookmark are never the same spot (different
// identity domains). Quotes are keyed by (spine, startWord, endWord); point bookmarks by
// pointKeyMatch above.
bool keyMatchFull(bool aQuote, uint16_t aSpine, uint16_t aPara, float aProg, uint16_t aStartW, uint16_t aEndW,
                  bool bQuote, uint16_t bSpine, uint16_t bPara, float bProg, uint16_t bStartW, uint16_t bEndW) {
  if (aQuote != bQuote) return false;
  if (aQuote) return aSpine == bSpine && aStartW == bStartW && aEndW == bEndW;
  return pointKeyMatch(aSpine, aPara, aProg, bSpine, bPara, bProg);
}

bool sameBookmark(const Bookmark& a, const Bookmark& b) {
  return keyMatchFull(a.quote, a.spineIndex, a.paragraphIndex, a.progress, a.startWord, a.endWord, b.quote,
                      b.spineIndex, b.paragraphIndex, b.progress, b.startWord, b.endWord);
}
bool sameTomb(const Tombstone& a, const Tombstone& b) {
  return keyMatchFull(a.quote, a.spineIndex, a.paragraphIndex, a.progress, a.startWord, a.endWord, b.quote,
                      b.spineIndex, b.paragraphIndex, b.progress, b.startWord, b.endWord);
}
bool tombHits(const Tombstone& t, const Bookmark& b) {
  return keyMatchFull(t.quote, t.spineIndex, t.paragraphIndex, t.progress, t.startWord, t.endWord, b.quote,
                      b.spineIndex, b.paragraphIndex, b.progress, b.startWord, b.endWord);
}

// Rewrite the book-path string embedded in a relocated bookmark .bin (written at
// writeToFile() below) to `newPath`. Without this, loadForBook()'s path-sanity
// check in readFromFile() rejects the renamed file as belonging to a different
// book (storedPath != bookFilePath) and the bookmarks silently fail to load.
// Header (version, count) + title/author strings + bookmark records are copied
// verbatim; only the path string changes, so this works across all file versions.
bool rewriteEmbeddedPath(const std::string& binPath, const std::string& newPath) {
  std::string header;  // version byte + count field, copied verbatim
  std::string title, author, oldPath;
  std::vector<uint8_t> records;

  {
    HalFile f;
    if (!Storage.openFileForRead("BKS", binPath, f)) return false;

    uint8_t version = 0;
    serialization::readPod(f, version);
    if (!isKnownVersion(version)) return false;

    const size_t countLen = (version == LEGACY_VERSION) ? sizeof(uint8_t) : sizeof(uint16_t);
    header.resize(1 + countLen);
    header[0] = static_cast<char>(version);
    if (f.read(&header[1], countLen) != static_cast<int>(countLen)) return false;

    if (!serialization::readString(f, title) || !serialization::readString(f, author) ||
        !serialization::readString(f, oldPath)) {
      return false;
    }

    const int remaining = f.available();
    if (remaining < 0) return false;
    records.resize(remaining);
    if (!records.empty() && f.read(records.data(), records.size()) != remaining) return false;

    f.close();  // must close before reopening the same path for write
  }

  HalFile out;
  if (!Storage.openFileForWrite("BKS", binPath, out)) return false;
  out.write(header.data(), header.size());
  serialization::writeString(out, title);
  serialization::writeString(out, author);
  serialization::writeString(out, newPath);
  if (!records.empty()) out.write(records.data(), records.size());
  return true;
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

  const uint32_t crc =
      esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(filePath.data()), static_cast<uint32_t>(filePath.size()));
  storeFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";

  // Tombstones live in a parallel file so the bookmark format stays untouched.
  tombstones.clear();
  tombDirty = false;
  tombFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".tomb";
  if (Storage.exists(tombFilePath.c_str())) readTombstones();

  // Full-preview sidecar for quotes (identity-keyed, append-only). No load here — read
  // lazily one entry at a time via readPreviewAt.
  qtextFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".qtext";

  // KOReader-XPath sidecar. Same lazy, identity-keyed treatment as .qtext — never loaded
  // here, read one entry at a time via getXPath.
  xpathFilePath = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".xpath";

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
  qtextFilePath.clear();
  xpathFilePath.clear();
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
    // Replace only an existing POINT bookmark on this page — quotes share the page but are
    // independent marks and must not be dropped by adding a page bookmark.
    std::erase_if(bookmarks, [&](const Bookmark& b) {
      return !b.quote && b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
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

BookmarkStore::AddResult BookmarkStore::addQuote(uint16_t spineIndex, float progress, uint16_t startWord,
                                                 uint16_t endWord, int pageCount, const char* chapterTitle,
                                                 const char* preview, int currentPage) {
  // Re-adding the exact same range replaces the prior quote (and its preview), rather
  // than dedup-by-page like point bookmarks — multiple quotes may share a page.
  std::erase_if(bookmarks, [&](const Bookmark& b) {
    return b.quote && b.spineIndex == spineIndex && b.startWord == startWord && b.endWord == endWord;
  });

  if (bookmarks.size() >= MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark limit (%u) reached", MAX_BOOKMARKS);
    return AddResult::LimitReached;
  }

  const std::string full = preview ? std::string(preview).substr(0, QUOTE_PREVIEW_MAX) : std::string();

  Bookmark bm{};
  bm.spineIndex = spineIndex;
  bm.progress = progress;
  bm.version = nextVersion();
  snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", chapterTitle ? chapterTitle : "");
  bm.paragraphIndex = UINT16_MAX;                                // quotes are keyed by word range, not paragraph anchor
  snprintf(bm.snippet, sizeof(bm.snippet), "%s", full.c_str());  // teaser for list + sync
  bm.returnMark = false;
  bm.chapterCurrentPage = static_cast<uint16_t>(currentPage < 0 ? 0 : currentPage);
  bm.chapterPageCount = static_cast<uint16_t>(pageCount < 0 ? 0 : pageCount);
  bm.quote = true;
  bm.endSpineIndex = spineIndex;  // single-page selection (cross-chapter is future work)
  bm.endProgress = progress;
  bm.startWord = startWord;
  bm.endWord = endWord;

  // Persist the full preview before the bookmark record, so a crash between the two
  // leaves an orphan preview entry (harmless, never matched) rather than a quote with
  // no recoverable text.
  appendPreview(spineIndex, startWord, endWord, full);

  bookmarks.push_back(bm);
  sortBookmarks();
  clearTombstoneFor(bm);
  dirty = true;
  saveToFile();
  exportTxt();
  return AddResult::Added;
}

bool BookmarkStore::removeQuoteByRange(uint16_t spineIndex, uint16_t startWord, uint16_t endWord) {
  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.quote && b.spineIndex == spineIndex && b.startWord == startWord && b.endWord == endWord;
  });
  if (it == bookmarks.end()) return false;

  addTombstone(*it);  // propagate the delete on next sync
  bookmarks.erase(it);
  dirty = true;
  saveToFile();
  compactPreviews();  // drop the orphaned preview entry (streamed, one at a time)
  compactXPaths();
  exportTxt();
  return true;
}

bool BookmarkStore::readPreviewAt(size_t index, std::string& out) const {
  out.clear();
  if (index >= bookmarks.size()) return false;
  const Bookmark& bm = bookmarks[index];
  if (!bm.quote) return false;
  return readPreviewForKey(bm.spineIndex, bm.startWord, bm.endWord, out);
}

void BookmarkStore::removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  // Page bookmark toggle only ever removes a POINT bookmark — never a quote, which is
  // removed deliberately from the bookmark list, not by the hold-left page toggle.
  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return !b.quote && b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
  if (it == bookmarks.end()) return;

  if (!it->returnMark) addTombstone(*it);  // device-only return marks never synced, so no tombstone
  bookmarks.erase(it);
  dirty = true;
  saveToFile();
  compactXPaths();  // points carry an xpath too, unlike previews
}

bool BookmarkStore::isForeignMark(const Bookmark& bm) {
  if (bm.quote) return bm.startWord >= FOREIGN_KEY_BASE;
  return bm.paragraphIndex >= FOREIGN_KEY_BASE && bm.paragraphIndex != UINT16_MAX;
}

size_t BookmarkStore::countUnplacedForeign() const {
  size_t n = 0;
  for (const auto& bm : bookmarks) {
    if (isUnplacedForeign(bm)) n++;
  }
  return n;
}

size_t BookmarkStore::indexOfMark(const bool quote, const uint16_t spineIndex, const uint16_t paragraphIndex,
                                  const float progress, const uint16_t startWord, const uint16_t endWord) const {
  for (size_t i = 0; i < bookmarks.size(); i++) {
    const Bookmark& b = bookmarks[i];
    if (keyMatchFull(quote, spineIndex, paragraphIndex, progress, startWord, endWord, b.quote, b.spineIndex,
                     b.paragraphIndex, b.progress, b.startWord, b.endWord)) {
      return i;
    }
  }
  return SIZE_MAX;
}

bool BookmarkStore::adoptForeignMark(const size_t index, const uint16_t spineIndex, const float progress,
                                     const uint16_t paragraphIndex, const char* chapterTitle, const std::string& xpath,
                                     const std::string& endXPath, const uint16_t page, const uint16_t pageCount,
                                     const uint32_t visibleTextOffset, const float endProgress) {
  if (index >= bookmarks.size()) return false;
  if (!isForeignMark(bookmarks[index])) return false;
  const char* title = (chapterTitle && *chapterTitle) ? chapterTitle : bookmarks[index].chapterTitle;

  if (bookmarks[index].quote) {
    Bookmark& bm = bookmarks[index];
    const bool moved = bm.progress != progress;
    const bool retitled = strncmp(bm.chapterTitle, title, sizeof(bm.chapterTitle)) != 0;
    // The peer sends no page numbers, so the list shows a mark with no "page X of Y"
    // until this fills them in -- which is also how the pair is spotted on the device.
    const bool repaged = pageCount > 0 && (bm.chapterCurrentPage != page || bm.chapterPageCount != pageCount);
    const bool reoffset = visibleTextOffset != 0 && bm.visibleTextOffset != visibleTextOffset;
    // A resolved end, or the start when none was resolved: a quote that ends where it
    // begins is drawn on one page, which is the behaviour before ends were resolved.
    const float end = endProgress >= 0.0f ? endProgress : progress;
    const bool restretched = bm.endProgress != end;
    if (!moved && !retitled && !repaged && !reoffset && !restretched) return false;  // already where it belongs
    bm.progress = progress;
    bm.endProgress = end;
    if (visibleTextOffset != 0) bm.visibleTextOffset = visibleTextOffset;
    if (pageCount > 0) {
      bm.chapterCurrentPage = page;
      bm.chapterPageCount = pageCount;
    }
    // Guarded: when no title was supplied, `title` aliases this very buffer.
    if (retitled) snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", title);
    // Deliberately NOT nextVersion(). Adopting a quote changes no identity field -- a
    // quote is keyed by (spine, startWord, endWord), and all three survive -- so there is
    // nothing here a peer has to be told. What it does change is this device's own
    // pagination: page numbers describe the font size, margins and screen that produced
    // them, and every peer re-derives progress and offset in its own adoption pass,
    // because a KOReader-born quote keeps its synthetic word range and so stays foreign.
    // Bumping anyway would raise the record above the last version the peers published,
    // and a peer's delete -- stamped one above what it could see -- then loses the
    // tie-break in mergeFrom and the highlight comes back from the dead.
    sortBookmarks();
    dirty = true;
    saveToFile();
    LOG_DBG("BKS", "Adopted foreign quote at spine %u -> progress %.4f", spineIndex, static_cast<double>(progress));
    return true;
  }

  // Copy what the re-add needs: removeBookmarkAt erases the record out from under it.
  char snippet[BOOKMARK_SNIPPET_MAX];
  snprintf(snippet, sizeof(snippet), "%s", bookmarks[index].snippet);
  char keptTitle[BOOKMARK_CHAPTER_TITLE_MAX];
  snprintf(keptTitle, sizeof(keptTitle), "%s", title);

  // Tombstone first. addBookmark() drops any existing point bookmark on the same page,
  // which would take the synthetic one with it and leave no tombstone — the peer would
  // then re-send it on every sync. pageCount 0 below disables that same dedup for the
  // record going in, so an unrelated mark sharing the page is not collateral.
  removeBookmarkAt(index);
  if (addBookmark(spineIndex, progress, 0, keptTitle, paragraphIndex, snippet, false, 0) != AddResult::Added) {
    LOG_ERR("BKS", "Adopting a foreign bookmark failed: the limit was reached");
    return false;
  }

  // Carry the anchor over rather than re-resolving it: the caller has already paid for
  // the chapter stream that produced it, and compactXPaths() dropped the old entry with
  // the synthetic record.
  for (const auto& bm : bookmarks) {
    // Progress as well as the anchor: when the XPath resolved to no paragraph the new
    // record is keyed by progress alone, and the paragraph test would then match any
    // other unanchored point bookmark in the same chapter.
    if (!bm.quote && bm.spineIndex == spineIndex && bm.paragraphIndex == paragraphIndex && bm.progress == progress) {
      setXPath(bm, xpath, endXPath);
      if (visibleTextOffset != 0) {
        const_cast<Bookmark&>(bm).visibleTextOffset = visibleTextOffset;
        dirty = true;
      }
      if (pageCount > 0) {
        // addBookmark took pageCount 0 to disable its same-page dedup, so the real page
        // numbers go on here, where they cannot cost an unrelated mark its record.
        Bookmark& placed = const_cast<Bookmark&>(bm);
        placed.chapterCurrentPage = page;
        placed.chapterPageCount = pageCount;
        dirty = true;
      }
      if (dirty) saveToFile();
      break;
    }
  }
  LOG_DBG("BKS", "Adopted foreign bookmark at spine %u -> paragraph %u", spineIndex, paragraphIndex);
  return true;
}

bool BookmarkStore::removeBookmarkAt(size_t index) {
  if (index >= bookmarks.size()) return false;

  const bool wasQuote = bookmarks[index].quote;
  if (!bookmarks[index].returnMark) addTombstone(bookmarks[index]);  // device-only: no tombstone for return marks
  bookmarks.erase(bookmarks.begin() + index);
  dirty = true;
  saveToFile();
  if (wasQuote) {
    compactPreviews();  // drop the orphaned preview entry
    exportTxt();
  }
  compactXPaths();  // unlike previews, points hold xpaths as well
  return true;
}

bool BookmarkStore::removeReturnMarkAt(uint16_t spineIndex, uint16_t paragraphIndex, float progress) {
  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    // Return marks are always point bookmarks, never quotes — point identity applies.
    return b.returnMark &&
           pointKeyMatch(b.spineIndex, b.paragraphIndex, b.progress, spineIndex, paragraphIndex, progress);
  });
  if (it == bookmarks.end()) return false;

  // Device-only mark: it was never pushed, so no tombstone is needed to propagate a delete.
  bookmarks.erase(it);
  dirty = true;
  saveToFile();
  return true;
}

bool BookmarkStore::removeReturnMarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  const float pageSlice = 1.0f / static_cast<float>(pageCount);
  const float pageStart = pageProgress;
  const float pageEnd = pageProgress + pageSlice;

  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.returnMark && b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
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

bool BookmarkStore::hasPointBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  const float pageSlice = 1.0f / static_cast<float>(pageCount);
  const float pageStart = pageProgress;
  const float pageEnd = pageProgress + pageSlice;
  return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return !b.quote && b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
}

bool BookmarkStore::hasQuoteForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  const float pageSlice = 1.0f / static_cast<float>(pageCount);
  const float pageStart = pageProgress;
  const float pageEnd = pageProgress + pageSlice;
  return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.quote && b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
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
    const Tombstone key{bm.spineIndex, bm.paragraphIndex, bm.progress, v, bm.quote, bm.startWord, bm.endWord};
    auto it = std::find_if(tombstones.begin(), tombstones.end(), [&](const Tombstone& e) { return sameTomb(e, key); });
    if (it != tombstones.end()) {
      it->version = v;
    } else {
      tombstones.push_back(key);
    }
    tombDirty = true;
  }
  saveTombstones();
  bookmarks.clear();
  dirty = false;
  // Nothing remains — drop both sidecars and refresh the .txt export.
  if (!qtextFilePath.empty() && Storage.exists(qtextFilePath.c_str())) Storage.remove(qtextFilePath.c_str());
  if (!xpathFilePath.empty() && Storage.exists(xpathFilePath.c_str())) Storage.remove(xpathFilePath.c_str());
  exportTxt();
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
  // Do NOT reject an over-cap count: the reserve below is bounded to MAX_BOOKMARKS and the
  // read loop stops there, so a legacy file written under the old 1024 cap (or a corrupt
  // count) loads its first MAX_BOOKMARKS records gracefully instead of failing the open or
  // aborting on a too-large contiguous allocation.
  if (count > MAX_BOOKMARKS) {
    LOG_DBG("BKS", "Bookmark count %u exceeds cap %u, loading first %u", count, MAX_BOOKMARKS, MAX_BOOKMARKS);
  }

  std::string tmp;
  // A false return means the length prefix exceeded the bytes left in the file —
  // the file is corrupt. Delete it so a clean store regenerates instead of
  // failing every open. (Closes f first; SdFat requires close before remove.)
  if (!serialization::readString(f, tmp) ||  // title
      !serialization::readString(f, tmp) ||  // author
      !serialization::readString(f, tmp)) {  // stored path
    LOG_ERR("BKS", "Corrupt bookmark file (bad string length), resetting: %s", storeFilePath.c_str());
    f.close();
    Storage.remove(storeFilePath.c_str());
    return false;
  }
  const std::string& storedPath = tmp;
  if (storedPath != bookFilePath) {
    // This file was located via crc(bookFilePath) (the current book's path), so it
    // belongs to this book -- a CRC32 collision with some other path is
    // astronomically unlikely. A mismatch here means the embedded path is stale
    // from a relocation done before relocateForFilePath() patched it (or before
    // that fix existed). Self-heal: accept the bookmarks and persist the corrected
    // path on the next save, instead of silently dropping them.
    LOG_ERR("BKS", "Bookmark file has stale embedded path '%s' (expected '%s'), self-healing", storedPath.c_str(),
            bookFilePath.c_str());
    dirty = true;
  }

  bookmarks.clear();
  bookmarks.reserve(std::min<size_t>(count, MAX_BOOKMARKS));
  for (uint16_t i = 0; i < count; i++) {
    if (bookmarks.size() >= MAX_BOOKMARKS) break;  // bounded load: ignore records past the cap
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
    if (version >= QUOTE_RANGE_VERSION) {
      uint8_t quoteFlag = 0;
      constexpr int kRangeBytes = static_cast<int>(sizeof(quoteFlag) + sizeof(bm.endSpineIndex) +
                                                   sizeof(bm.endProgress) + sizeof(bm.startWord) + sizeof(bm.endWord));
      if (f.available() < kRangeBytes) {
        LOG_ERR("BKS", "Bookmark file truncated at quote range, record %u", i);
        return false;
      }
      serialization::readPod(f, quoteFlag);
      bm.quote = quoteFlag != 0;
      serialization::readPod(f, bm.endSpineIndex);
      serialization::readPod(f, bm.endProgress);
      serialization::readPod(f, bm.startWord);
      serialization::readPod(f, bm.endWord);
    } else {
      bm.quote = false;  // pre-v8 records are all point bookmarks
    }
    if (version >= VISIBLE_OFFSET_VERSION) {
      if (f.available() < static_cast<int>(sizeof(bm.visibleTextOffset))) {
        LOG_ERR("BKS", "Bookmark file truncated at visible offset, record %u", i);
        return false;
      }
      serialization::readPod(f, bm.visibleTextOffset);
    } else {
      bm.visibleTextOffset = 0;  // pre-v9: fall back to the paragraph anchor as before
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
    const uint8_t quoteFlag = bm.quote ? 1 : 0;
    serialization::writePod(f, quoteFlag);
    serialization::writePod(f, bm.endSpineIndex);
    serialization::writePod(f, bm.endProgress);
    serialization::writePod(f, bm.startWord);
    serialization::writePod(f, bm.endWord);
    serialization::writePod(f, bm.visibleTextOffset);
  }

  LOG_DBG("BKS", "Saved %u bookmark(s)", count);
  return true;
}

// Sync blob key map. The wire format uses short keys to cut body size (~half of the
// old body was repeated long key names) — this directly raises how many bookmarks fit
// under the TLS PUT's contiguous-buffer ceiling. parseFromJson reads BOTH the short keys
// and the legacy long keys, so a peer on older firmware (long keys) still merges; once
// every device is updated only short keys travel.
//
//   wrapper:  b=bookmarks   t=tombstones
//   bookmark: s=spineIndex  p=progress  v=version  ct=chapterTitle  pi=paragraphIndex
//             sn=snippet    cp=chapterCurrentPage  pc=chapterPageCount
//             q=quote  es=endSpineIndex  ep=endProgress  sw=startWord  ew=endWord
//   tombstone:s=spineIndex  pi=paragraphIndex  p=progress  v=version
//             q=quote  sw=startWord  ew=endWord
// One builder for both the plain and the anchored blob. `anchorStore`/`anchorOffsets`, when
// given, name a sidecar and the per-bookmark offsets indexXPaths() found in it; each anchor
// is read, copied into the document and released before the next, so the whole set is never
// resident. Building the document twice (or building it and re-parsing it) would double the
// peak on the one path where the heap is tightest, which is why this is shared rather than
// layered.
std::string BookmarkStore::serializeInternal(const std::vector<Bookmark>& bms, const std::vector<Tombstone>& tombs,
                                             size_t maxBodyBytes, const BookmarkStore* anchorStore,
                                             const uint32_t* anchorOffsets) {
  JsonDocument doc;
  std::string anchorStart;
  std::string anchorEnd;
  JsonArray arr = doc["b"].to<JsonArray>();
  for (size_t i = 0; i < bms.size(); i++) {
    const auto& bm = bms[i];
    if (bm.returnMark) continue;  // session "return here" mark is device-only — never synced
    JsonObject obj = arr.add<JsonObject>();
    obj["s"] = bm.spineIndex;
    obj["p"] = bm.progress;
    obj["v"] = bm.version;  // Lamport version (formerly the always-0 "timestamp")
    obj["ct"] = bm.chapterTitle;
    obj["pi"] = bm.paragraphIndex;
    obj["sn"] = bm.snippet;
    obj["cp"] = bm.chapterCurrentPage;
    obj["pc"] = bm.chapterPageCount;
    // Quote range. Only the 64-char snippet teaser crosses the wire (above); the full
    // preview text stays device-local in .qtext and is never synced. Omitted for point
    // bookmarks to keep their blob minimal.
    if (bm.quote) {
      obj["q"] = true;
      obj["es"] = bm.endSpineIndex;
      obj["ep"] = bm.endProgress;
      obj["sw"] = bm.startWord;
      obj["ew"] = bm.endWord;
    }
    // The KOReader anchor: "xp" locates the mark, "xp1" the far end of a highlight. These
    // are what a crengine client places an annotation from — CrossPoint's own spine/word
    // coordinates mean nothing to it. Absent for a mark the device could not resolve.
    if (anchorStore && anchorOffsets && anchorOffsets[i] != 0 &&
        anchorStore->readXPathAt(anchorOffsets[i], anchorStart, anchorEnd)) {
      obj["xp"] = anchorStart;
      if (!anchorEnd.empty()) obj["xp1"] = anchorEnd;
    }
  }
  JsonArray tarr = doc["t"].to<JsonArray>();
  for (const auto& t : tombs) {
    JsonObject obj = tarr.add<JsonObject>();
    obj["s"] = t.spineIndex;
    obj["pi"] = t.paragraphIndex;
    obj["p"] = t.progress;
    obj["v"] = t.version;
    if (t.quote) {
      obj["q"] = true;
      obj["sw"] = t.startWord;
      obj["ew"] = t.endWord;
    }
  }
  // If building the tree exhausted the (nothrow) ArduinoJson allocator, the doc is
  // truncated — emit nothing rather than a malformed/partial blob the server would
  // store. Caller treats an empty result as "skip upload".
  if (doc.overflowed()) {
    LOG_ERR("BKS", "Bookmark serialize: JsonDocument overflow (low heap) — skipping upload");
    return std::string();
  }
  // Abort-safety: measureJson() allocates nothing, so the exact body length is known
  // before any allocation. out.reserve() OOM-aborts under -fno-exceptions, and this
  // path now runs inside a keep-alive sync session (the bookmark GET's mbedTLS arena is
  // still held), so the caller passes the live largest-contiguous-block budget. Over
  // budget -> emit nothing; the caller treats empty as "skip upload" rather than wiping
  // the server set, and nothing aborts.
  const size_t bodyLen = measureJson(doc) + 1;
  if (bodyLen > maxBodyBytes) {
    LOG_ERR("BKS", "Bookmark serialize: body %u > budget %u (low heap) — skipping upload", (unsigned)bodyLen,
            (unsigned)maxBodyBytes);
    return std::string();
  }
  std::string out;
  // Reserve the exact serialized length up front. Without this, std::string grows by
  // doubling-realloc (alloc new, copy, free old) several times for a multi-KB blob —
  // three heap ops per growth that fragment DRAM right before the TLS PUT needs a
  // contiguous send buffer. One sized allocation instead.
  out.reserve(bodyLen);
  serializeJson(doc, out);
  return out;
}

std::string BookmarkStore::serializeToJson(const std::vector<Bookmark>& bms, const std::vector<Tombstone>& tombs,
                                           const size_t maxBodyBytes) {
  return serializeInternal(bms, tombs, maxBodyBytes, nullptr, nullptr);
}

std::string BookmarkStore::serializeWithAnchors(const size_t maxBodyBytes) const {
  const size_t n = std::min<size_t>(bookmarks.size(), MAX_BOOKMARKS);
  if (n == 0 || n < bookmarks.size()) {
    // Over the cap the offsets array cannot index every mark, and a partly anchored blob
    // is not worth the risk of a mis-indexed one.
    return serializeToJson(bookmarks, tombstones, maxBodyBytes);
  }

  // 512 bytes for the whole book, against ~18KB if every anchor were held resident.
  uint32_t offsets[MAX_BOOKMARKS];
  if (indexXPaths(offsets, n) == 0) {
    return serializeToJson(bookmarks, tombstones, maxBodyBytes);  // nothing anchored yet
  }
  return serializeInternal(bookmarks, tombstones, maxBodyBytes, this, offsets);
}

bool BookmarkStore::parseFromJson(const char* json, std::vector<Bookmark>& outBms, std::vector<Tombstone>& outTombs,
                                  bool* outTruncated) {
  outBms.clear();
  outTombs.clear();
  if (outTruncated) *outTruncated = false;
  if (json == nullptr || json[0] == '\0') return true;  // empty blob = nothing stored

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    LOG_ERR("BKS", "Bookmark sync JSON parse error: %s", err.c_str());
    return false;
  }

  // Accepts three shapes for back-compat: the short-key form ("b"), the older long-key
  // form ("bookmarks"), and the legacy bare-array (just the bookmark list). Each field is
  // read short-key-first with a long-key fallback, so a peer still on long-key firmware
  // merges correctly.
  JsonArray arr = doc.is<JsonArray>()        ? doc.as<JsonArray>()
                  : doc["b"].is<JsonArray>() ? doc["b"].as<JsonArray>()
                                             : doc["bookmarks"].as<JsonArray>();
  // Bound the reserve to the cap actually accepted below. A hostile/oversized remote blob
  // could otherwise reserve a huge vector and, under -fno-exceptions, abort() on bad_alloc.
  outBms.reserve(std::min<size_t>(arr.size(), MAX_BOOKMARKS));
  for (JsonObject obj : arr) {
    if (outBms.size() >= MAX_BOOKMARKS) break;
    Bookmark bm{};
    bm.spineIndex = obj["s"] | (obj["spineIndex"] | static_cast<uint16_t>(0));
    bm.progress = obj["p"] | (obj["progress"] | 0.0f);
    // Short "v" / long "version"; fall back to the legacy "timestamp" key (oldest blobs).
    bm.version = obj["v"] | (obj["version"] | (obj["timestamp"] | static_cast<uint32_t>(0)));
    bm.paragraphIndex = obj["pi"] | (obj["paragraphIndex"] | static_cast<uint16_t>(UINT16_MAX));
    snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", obj["ct"] | (obj["chapterTitle"] | ""));
    snprintf(bm.snippet, sizeof(bm.snippet), "%s", obj["sn"] | (obj["snippet"] | ""));
    // Display-only page snapshot; absent from older/other-firmware blobs → 0 (unknown).
    bm.chapterCurrentPage = obj["cp"] | (obj["chapterCurrentPage"] | static_cast<uint16_t>(0));
    bm.chapterPageCount = obj["pc"] | (obj["chapterPageCount"] | static_cast<uint16_t>(0));
    // Quote range (absent for point bookmarks and pre-v8 peers → defaults to a point).
    bm.quote = obj["q"] | (obj["quote"] | false);
    bm.endSpineIndex = obj["es"] | (obj["endSpineIndex"] | bm.spineIndex);
    bm.endProgress = obj["ep"] | (obj["endProgress"] | bm.progress);
    bm.startWord = obj["sw"] | (obj["startWord"] | static_cast<uint16_t>(0));
    bm.endWord = obj["ew"] | (obj["endWord"] | static_cast<uint16_t>(0));
    outBms.push_back(bm);
  }

  JsonArray tarr = doc["t"].is<JsonArray>() ? doc["t"].as<JsonArray>() : doc["tombstones"].as<JsonArray>();
  outTombs.reserve(std::min<size_t>(tarr.size(), MAX_BOOKMARKS));
  for (JsonObject obj : tarr) {
    if (outTombs.size() >= MAX_BOOKMARKS) break;
    Tombstone t{};
    t.spineIndex = obj["s"] | (obj["spineIndex"] | static_cast<uint16_t>(0));
    t.paragraphIndex = obj["pi"] | (obj["paragraphIndex"] | static_cast<uint16_t>(UINT16_MAX));
    t.progress = obj["p"] | (obj["progress"] | 0.0f);
    t.version = obj["v"] | (obj["version"] | static_cast<uint32_t>(0));
    t.quote = obj["q"] | (obj["quote"] | false);
    t.startWord = obj["sw"] | (obj["startWord"] | static_cast<uint16_t>(0));
    t.endWord = obj["ew"] | (obj["endWord"] | static_cast<uint16_t>(0));
    outTombs.push_back(t);
  }

  // Either cap hit means the server holds records this device cannot represent. The
  // caller must not PUT back what it parsed — that blob would drop them for every peer.
  if (outTruncated && (arr.size() > MAX_BOOKMARKS || tarr.size() > MAX_BOOKMARKS)) {
    *outTruncated = true;
    LOG_ERR("BKS", "Remote blob truncated: %u bookmarks / %u tombstones exceed cap %u", (unsigned)arr.size(),
            (unsigned)tarr.size(), (unsigned)MAX_BOOKMARKS);
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
  if (version != TOMB_LEGACY_VERSION && version != TOMB_VERSION_V2 && version != TOMB_VERSION) {
    LOG_ERR("BKS", "Unknown tombstone file version: %u", version);
    return false;
  }
  if (f.available() < static_cast<int>(sizeof(uint16_t))) return false;
  uint16_t count = 0;
  serialization::readPod(f, count);
  if (count > MAX_BOOKMARKS) {
    LOG_DBG("BKS", "Tombstone count %u exceeds cap %u, loading first %u", count, MAX_BOOKMARKS, MAX_BOOKMARKS);
  }

  tombstones.clear();
  tombstones.reserve(std::min<size_t>(count, MAX_BOOKMARKS));
  int recordSize = static_cast<int>(sizeof(uint16_t) + sizeof(uint16_t) + sizeof(float));
  if (version >= TOMB_VERSION_V2) recordSize += static_cast<int>(sizeof(uint32_t));
  if (version >= TOMB_VERSION) recordSize += static_cast<int>(sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t));
  for (uint16_t i = 0; i < count; i++) {
    if (tombstones.size() >= MAX_BOOKMARKS) break;  // bounded load
    if (f.available() < recordSize) {
      LOG_ERR("BKS", "Tombstone file truncated at record %u", i);
      return false;
    }
    Tombstone t{};
    serialization::readPod(f, t.spineIndex);
    serialization::readPod(f, t.paragraphIndex);
    serialization::readPod(f, t.progress);
    if (version >= TOMB_VERSION_V2) {
      serialization::readPod(f, t.version);
    } else {
      t.version = 0;  // legacy tombstone: lowest priority, loses to any stamped edit
    }
    if (version >= TOMB_VERSION) {
      uint8_t quoteFlag = 0;
      serialization::readPod(f, quoteFlag);
      t.quote = quoteFlag != 0;
      serialization::readPod(f, t.startWord);
      serialization::readPod(f, t.endWord);
    } else {
      t.quote = false;  // pre-v3 tombstones are all point bookmarks
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
    const uint8_t quoteFlag = t.quote ? 1 : 0;
    serialization::writePod(f, quoteFlag);
    serialization::writePod(f, t.startWord);
    serialization::writePod(f, t.endWord);
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
  const Tombstone key{bm.spineIndex, bm.paragraphIndex, bm.progress, v, bm.quote, bm.startWord, bm.endWord};
  for (auto& e : tombstones) {
    if (sameTomb(e, key)) {
      e.version = v;  // refresh an existing tombstone so a re-delete still outranks
      tombDirty = true;
      saveTombstones();
      return;
    }
  }
  tombstones.push_back(key);
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
    bool quote;
    uint16_t spineIndex;
    uint16_t paragraphIndex;
    float progress;
    uint16_t startWord;
    uint16_t endWord;
    uint32_t version;
    const Bookmark* bm;  // non-null => bookmark wins this spot; null => tombstone wins
  };
  std::vector<Winner> winners;
  winners.reserve(bookmarks.size() + tombstones.size() + remoteBookmarks.size() + remoteTombstones.size());

  const auto consider = [&](bool quote, uint16_t spine, uint16_t para, float prog, uint16_t startW, uint16_t endW,
                            uint32_t version, const Bookmark* bm) {
    observeVersion(version);  // advance the local clock past everything we see
    for (auto& w : winners) {
      if (keyMatchFull(w.quote, w.spineIndex, w.paragraphIndex, w.progress, w.startWord, w.endWord, quote, spine, para,
                       prog, startW, endW)) {
        const bool better = version > w.version || (version == w.version && bm != nullptr && w.bm == nullptr);
        if (better) {
          w.version = version;
          w.bm = bm;
        }
        return;
      }
    }
    winners.push_back(Winner{quote, spine, para, prog, startW, endW, version, bm});
  };

  for (const auto& b : bookmarks)
    consider(b.quote, b.spineIndex, b.paragraphIndex, b.progress, b.startWord, b.endWord, b.version, &b);
  for (const auto& t : tombstones)
    consider(t.quote, t.spineIndex, t.paragraphIndex, t.progress, t.startWord, t.endWord, t.version, nullptr);
  for (const auto& b : remoteBookmarks)
    consider(b.quote, b.spineIndex, b.paragraphIndex, b.progress, b.startWord, b.endWord, b.version, &b);
  for (const auto& t : remoteTombstones)
    consider(t.quote, t.spineIndex, t.paragraphIndex, t.progress, t.startWord, t.endWord, t.version, nullptr);

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
      newTombstones.push_back(
          Tombstone{w.spineIndex, w.paragraphIndex, w.progress, w.version, w.quote, w.startWord, w.endWord});
    }
  }

  // Count bookmarks now present that weren't local before (for the return/log).
  size_t added = 0;
  for (const auto& nb : newBookmarks) {
    const bool wasLocal =
        std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& l) { return sameBookmark(l, nb); });
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
    // A merge may have dropped local quotes (remote tombstone won); reclaim their
    // preview entries. Merged-in remote quotes carry no preview (snippet-only sync), so
    // their .qtext entry is simply absent and QuoteViewer falls back to the snippet.
    compactPreviews();
    compactXPaths();  // same for anchors of bookmarks the merge dropped
    exportTxt();
  }
  if (tombsChanged) {
    tombDirty = true;
    saveTombstones();
  }
  return added;
}

void BookmarkStore::relocateForFilePath(const std::string& srcPath, const std::string& dstPath,
                                        const std::string& bookType) {
  if (srcPath == dstPath) return;
  const uint32_t srcCrc =
      esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(srcPath.data()), static_cast<uint32_t>(srcPath.size()));
  const uint32_t dstCrc =
      esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(dstPath.data()), static_cast<uint32_t>(dstPath.size()));
  const std::string srcBase = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(srcCrc);
  const std::string dstBase = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(dstCrc);

  // Tombstone sidecar: plain rename, no embedded path to fix.
  const std::string srcTomb = srcBase + ".tomb";
  if (Storage.exists(srcTomb.c_str())) {
    const std::string dstTomb = dstBase + ".tomb";
    // Overwrite-move (e.g. WebDAV MOVE onto an existing book path): Storage.rename
    // fails if dst exists, so the moved book's sidecars would be lost. The
    // destination book is being replaced, so drop its stale sidecar first.
    if (Storage.exists(dstTomb.c_str())) Storage.remove(dstTomb.c_str());
    if (Storage.rename(srcTomb.c_str(), dstTomb.c_str())) {
      LOG_DBG("BKS", "Relocated %s -> %s", srcTomb.c_str(), dstTomb.c_str());
    } else {
      LOG_ERR("BKS", "Failed to relocate %s -> %s (non-fatal)", srcTomb.c_str(), dstTomb.c_str());
    }
  }

  // Preview sidecar: plain rename, no embedded path (identity-keyed by spine/word range).
  const std::string srcQtext = srcBase + ".qtext";
  if (Storage.exists(srcQtext.c_str())) {
    const std::string dstQtext = dstBase + ".qtext";
    if (Storage.exists(dstQtext.c_str())) Storage.remove(dstQtext.c_str());
    if (Storage.rename(srcQtext.c_str(), dstQtext.c_str())) {
      LOG_DBG("BKS", "Relocated %s -> %s", srcQtext.c_str(), dstQtext.c_str());
    } else {
      LOG_ERR("BKS", "Failed to relocate %s -> %s (non-fatal)", srcQtext.c_str(), dstQtext.c_str());
    }
  }

  // Bookmark file: rename, then patch its embedded book path (see writeToFile())
  // to dstPath so loadForBook()'s path-sanity check doesn't reject the relocated
  // file as belonging to a different book.
  const std::string srcBin = srcBase + ".bin";
  if (!Storage.exists(srcBin.c_str())) return;
  const std::string dstBin = dstBase + ".bin";
  if (Storage.exists(dstBin.c_str())) Storage.remove(dstBin.c_str());  // overwrite-move: see .tomb above
  if (!Storage.rename(srcBin.c_str(), dstBin.c_str())) {
    LOG_ERR("BKS", "Failed to relocate %s -> %s (non-fatal)", srcBin.c_str(), dstBin.c_str());
    return;
  }
  LOG_DBG("BKS", "Relocated %s -> %s", srcBin.c_str(), dstBin.c_str());
  if (!rewriteEmbeddedPath(dstBin, dstPath)) {
    LOG_ERR("BKS", "Failed to patch embedded path in %s (non-fatal)", dstBin.c_str());
  }
}

void BookmarkStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc =
      esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(filePath.data()), static_cast<uint32_t>(filePath.size()));
  const std::string base = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc);
  // Remove the bookmark file and its tombstone + preview sidecars.
  for (const std::string& path : {base + ".bin", base + ".tomb", base + ".qtext"}) {
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

// ---- Full-preview (.qtext) store ----
// Entry layout (little-endian, repeated to EOF after a 1-byte version header):
//   [u16 spineIndex][u16 startWord][u16 endWord][u16 len][len bytes]
// Append-only on add; the latest entry for a key wins on read. Orphaned/superseded
// entries are reclaimed by compactPreviews(). Never holds more than one preview in RAM.

bool BookmarkStore::appendPreview(uint16_t spineIndex, uint16_t startWord, uint16_t endWord,
                                  const std::string& text) const {
  if (qtextFilePath.empty()) return false;
  Storage.mkdir(BOOKMARKS_DIR);
  const bool existed = Storage.exists(qtextFilePath.c_str());
  HalFile f;
  if (!Storage.openFileForAppend("BKS", qtextFilePath.c_str(), f)) {
    LOG_ERR("BKS", "Failed to open preview file for append");
    return false;
  }
  if (!existed) serialization::writePod(f, QTEXT_VERSION);
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(text.size(), QUOTE_PREVIEW_MAX));
  serialization::writePod(f, spineIndex);
  serialization::writePod(f, startWord);
  serialization::writePod(f, endWord);
  serialization::writePod(f, len);
  if (len) f.write(text.data(), len);
  return true;
}

bool BookmarkStore::readPreviewForKey(uint16_t spineIndex, uint16_t startWord, uint16_t endWord,
                                      std::string& out) const {
  out.clear();
  if (qtextFilePath.empty() || !Storage.exists(qtextFilePath.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead("BKS", qtextFilePath, f)) return false;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != QTEXT_VERSION) return false;

  bool found = false;
  constexpr int kHeaderBytes = static_cast<int>(4 * sizeof(uint16_t));
  while (f.available() >= kHeaderBytes) {
    uint16_t s, sw, ew, len;
    serialization::readPod(f, s);
    serialization::readPod(f, sw);
    serialization::readPod(f, ew);
    serialization::readPod(f, len);
    if (len > QUOTE_PREVIEW_MAX || f.available() < static_cast<int>(len)) break;  // truncated/corrupt
    if (s == spineIndex && sw == startWord && ew == endWord) {
      out.resize(len);
      if (len) f.read(&out[0], len);
      found = true;  // keep scanning — a later append for the same key supersedes this one
    } else if (len) {
      f.seekCur(static_cast<int64_t>(len));  // skip the payload of a non-matching entry
    }
  }
  return found;
}

void BookmarkStore::compactPreviews() const {
  if (qtextFilePath.empty() || !Storage.exists(qtextFilePath.c_str())) return;

  const bool anyQuote = std::any_of(bookmarks.begin(), bookmarks.end(), [](const Bookmark& b) { return b.quote; });
  if (!anyQuote) {
    Storage.remove(qtextFilePath.c_str());  // no quotes left — drop the sidecar entirely
    return;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(QUOTE_PREVIEW_MAX);
  if (!buf) {
    LOG_ERR("BKS", "OOM: %u bytes for preview compaction", static_cast<unsigned>(QUOTE_PREVIEW_MAX));
    return;
  }

  const std::string tmpPath = qtextFilePath + ".tmp";
  bool ok = false;
  {
    HalFile in;
    if (!Storage.openFileForRead("BKS", qtextFilePath, in)) return;
    uint8_t version = 0;
    serialization::readPod(in, version);
    if (version != QTEXT_VERSION) return;

    HalFile out;
    if (!Storage.openFileForWrite("BKS", tmpPath, out)) return;
    serialization::writePod(out, QTEXT_VERSION);

    constexpr int kHeaderBytes = static_cast<int>(4 * sizeof(uint16_t));
    while (in.available() >= kHeaderBytes) {
      uint16_t s, sw, ew, len;
      serialization::readPod(in, s);
      serialization::readPod(in, sw);
      serialization::readPod(in, ew);
      serialization::readPod(in, len);
      if (len > QUOTE_PREVIEW_MAX || in.available() < static_cast<int>(len)) break;  // truncated/corrupt
      if (len) in.read(buf.get(), len);
      const bool live = std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
        return b.quote && b.spineIndex == s && b.startWord == sw && b.endWord == ew;
      });
      if (live) {
        serialization::writePod(out, s);
        serialization::writePod(out, sw);
        serialization::writePod(out, ew);
        serialization::writePod(out, len);
        if (len) out.write(buf.get(), len);
      }
    }
    in.close();  // must close both before remove/rename on the same paths
    out.close();
    ok = true;
  }
  if (!ok) return;
  Storage.remove(qtextFilePath.c_str());
  Storage.rename(tmpPath.c_str(), qtextFilePath.c_str());
}

// --- KOReader XPath sidecar -------------------------------------------------
// Record: [u8 quote][u16 spine][u16 para][f32 progress][u16 startWord][u16 endWord]
//         [u16 len][u16 endLen][len bytes][endLen bytes]
// The five key fields are exactly what keyMatchFull() compares, so a stored entry is
// matched by the same rule the merge uses — an entry never binds to a different spot than
// the one it was written for. The second path is a highlight's end anchor; a point
// bookmark stores endLen 0.
namespace {
// Grouped in field order: quote | spine, para | progress | startWord, endWord, len, endLen.
constexpr int kXPathHeaderBytes =
    static_cast<int>(sizeof(uint8_t) + 2 * sizeof(uint16_t) + sizeof(float) + 4 * sizeof(uint16_t));

struct XPathKey {
  uint8_t quote;
  uint16_t spine;
  uint16_t para;
  float progress;
  uint16_t startWord;
  uint16_t endWord;
};

void readXPathKey(HalFile& f, XPathKey& k, uint16_t& len, uint16_t& endLen) {
  serialization::readPod(f, k.quote);
  serialization::readPod(f, k.spine);
  serialization::readPod(f, k.para);
  serialization::readPod(f, k.progress);
  serialization::readPod(f, k.startWord);
  serialization::readPod(f, k.endWord);
  serialization::readPod(f, len);
  serialization::readPod(f, endLen);
}

void writeXPathKey(HalFile& f, const XPathKey& k, uint16_t len, uint16_t endLen) {
  serialization::writePod(f, k.quote);
  serialization::writePod(f, k.spine);
  serialization::writePod(f, k.para);
  serialization::writePod(f, k.progress);
  serialization::writePod(f, k.startWord);
  serialization::writePod(f, k.endWord);
  serialization::writePod(f, len);
  serialization::writePod(f, endLen);
}

// A record's payload is the two paths back to back; both lengths are validated before
// either is trusted, so a corrupt tail ends the scan instead of seeking past the file.
bool xpathLensValid(HalFile& f, const uint16_t len, const uint16_t endLen) {
  return len <= BOOKMARK_XPATH_MAX && endLen <= BOOKMARK_XPATH_MAX &&
         f.available() >= static_cast<int>(len) + static_cast<int>(endLen);
}

bool xpathKeyHits(const XPathKey& k, const Bookmark& b) {
  return keyMatchFull(k.quote != 0, k.spine, k.para, k.progress, k.startWord, k.endWord, b.quote, b.spineIndex,
                      b.paragraphIndex, b.progress, b.startWord, b.endWord);
}

// Anchors are a cache, so a sidecar written by another firmware version is dropped rather
// than migrated. Without this, appending a current record to an older file would leave a
// mixed one that every reader rejects and every write grows.
void discardForeignXPathVersionAt(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return;
  uint8_t version = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead("BKS", path, f)) return;
    serialization::readPod(f, version);
  }  // must close before removing the same path
  if (version == XPATH_VERSION) return;
  LOG_DBG("BKS", "Dropping xpath sidecar written at version %u (current %u)", version, XPATH_VERSION);
  Storage.remove(path.c_str());
}

XPathKey xpathKeyOf(const Bookmark& b) {
  return XPathKey{
      static_cast<uint8_t>(b.quote ? 1 : 0), b.spineIndex, b.paragraphIndex, b.progress, b.startWord, b.endWord};
}

// Every key the sidecar already holds, in one pass with the payloads skipped. Keyed off
// the file rather than off getBookmarks() because adoptRemoteXPaths deliberately runs
// before the bookmark set is loaded.
size_t collectXPathKeys(const std::string& path, XPathKey* out, const size_t cap) {
  if (path.empty() || !Storage.exists(path.c_str())) return 0;
  HalFile f;
  if (!Storage.openFileForRead("BKS", path, f)) return 0;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != XPATH_VERSION) return 0;
  size_t n = 0;
  while (n < cap && f.available() >= kXPathHeaderBytes) {
    uint16_t len = 0;
    uint16_t endLen = 0;
    readXPathKey(f, out[n], len, endLen);
    if (!xpathLensValid(f, len, endLen)) break;  // truncated/corrupt
    if (len || endLen) f.seekCur(static_cast<int64_t>(len) + static_cast<int64_t>(endLen));
    n++;
  }
  return n;
}
}  // namespace

bool BookmarkStore::setXPath(const Bookmark& bm, const std::string& xpath, const std::string& endXPath) const {
  return appendXPathTo(xpathFilePath, bm, xpath, endXPath);
}

// Path-keyed rather than member-keyed: adoptRemoteXPaths writes a book's anchors with no
// book loaded, so the sidecar it targets cannot come from the instance.
bool BookmarkStore::appendXPathTo(const std::string& path, const Bookmark& bm, const std::string& xpath,
                                  const std::string& endXPath) {
  if (path.empty() || xpath.empty()) return false;
  Storage.mkdir(BOOKMARKS_DIR);
  discardForeignXPathVersionAt(path);
  const bool existed = Storage.exists(path.c_str());
  HalFile f;
  if (!Storage.openFileForAppend("BKS", path.c_str(), f)) {
    LOG_ERR("BKS", "Failed to open xpath file for append");
    return false;
  }
  if (!existed) serialization::writePod(f, XPATH_VERSION);
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(xpath.size(), BOOKMARK_XPATH_MAX));
  const uint16_t endLen = static_cast<uint16_t>(std::min<size_t>(endXPath.size(), BOOKMARK_XPATH_MAX));
  writeXPathKey(f, xpathKeyOf(bm), len, endLen);
  f.write(xpath.data(), len);
  if (endLen) f.write(endXPath.data(), endLen);
  return true;
}

size_t BookmarkStore::adoptRemoteXPaths(const char* json, const std::string& filePath, const char* bookType) {
  if (json == nullptr || json[0] == '\0') return 0;

  const uint32_t crc =
      esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(filePath.data()), static_cast<uint32_t>(filePath.size()));
  // Built here rather than read off the instance: this runs with no book loaded, and must
  // not depend on whether one happens to be.
  const std::string sidecar = std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".xpath";
  discardForeignXPathVersionAt(sidecar);

  // What is already anchored, so a re-sync does not append the same record again — the
  // sidecar is append-only, so a duplicate is dead weight that never stops accumulating.
  auto seen = makeUniqueNoThrow<XPathKey[]>(MAX_BOOKMARKS);
  if (!seen) {
    LOG_ERR("BKS", "OOM: %u bytes for the anchor key set", static_cast<unsigned>(MAX_BOOKMARKS * sizeof(XPathKey)));
    return 0;
  }
  size_t seenCount = collectXPathKeys(sidecar, seen.get(), MAX_BOOKMARKS);

  JsonDocument doc;
  if (deserializeJson(doc, json)) return 0;  // parseFromJson logs the parse error already
  JsonArray arr = doc.is<JsonArray>()        ? doc.as<JsonArray>()
                  : doc["b"].is<JsonArray>() ? doc["b"].as<JsonArray>()
                                             : doc["bookmarks"].as<JsonArray>();

  size_t adopted = 0;
  size_t index = 0;
  for (JsonObject obj : arr) {
    if (index++ >= MAX_BOOKMARKS) break;  // the same cap parseFromJson stops at
    const char* xp = obj["xp"] | "";
    if (xp[0] == '\0') continue;
    // Dropped, not truncated: a cut XPath resolves to nothing, and storing one would hand
    // the peer back a broken anchor with this device's authority behind it.
    const size_t xpLen = strnlen(xp, BOOKMARK_XPATH_MAX + 1);
    if (xpLen > BOOKMARK_XPATH_MAX) continue;

    // Identity fields only: the anchor key reads nothing else, and the merge -- not this --
    // decides whether the bookmark itself is kept.
    Bookmark bm{};
    bm.spineIndex = obj["s"] | (obj["spineIndex"] | static_cast<uint16_t>(0));
    bm.progress = obj["p"] | (obj["progress"] | 0.0f);
    bm.paragraphIndex = obj["pi"] | (obj["paragraphIndex"] | static_cast<uint16_t>(UINT16_MAX));
    bm.quote = obj["q"] | (obj["quote"] | false);
    bm.startWord = obj["sw"] | (obj["startWord"] | static_cast<uint16_t>(0));
    bm.endWord = obj["ew"] | (obj["endWord"] | static_cast<uint16_t>(0));

    bool have = false;
    for (size_t i = 0; i < seenCount && !have; i++) have = xpathKeyHits(seen[i], bm);
    if (have) continue;

    const char* xp1 = obj["xp1"] | "";
    const size_t xp1Len = strnlen(xp1, BOOKMARK_XPATH_MAX + 1);
    const std::string endXPath = xp1Len > BOOKMARK_XPATH_MAX ? std::string() : std::string(xp1, xp1Len);
    if (!appendXPathTo(sidecar, bm, std::string(xp, xpLen), endXPath)) continue;
    // Remember it too, so a blob naming one spot twice appends once.
    if (seenCount < MAX_BOOKMARKS) seen[seenCount++] = xpathKeyOf(bm);
    adopted++;
  }

  if (adopted > 0) {
    LOG_DBG("BKS", "Adopted %u anchor(s) from the remote blob", static_cast<unsigned>(adopted));
    SdDebugLog::log("BKS", "adopted %u remote anchor(s)", static_cast<unsigned>(adopted));
  }
  return adopted;
}

void BookmarkStore::discardForeignXPathVersion() const { discardForeignXPathVersionAt(xpathFilePath); }

bool BookmarkStore::getXPath(const Bookmark& bm, std::string& out, std::string* outEnd) const {
  out.clear();
  if (outEnd) outEnd->clear();
  if (xpathFilePath.empty() || !Storage.exists(xpathFilePath.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead("BKS", xpathFilePath, f)) return false;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != XPATH_VERSION) return false;

  bool found = false;
  while (f.available() >= kXPathHeaderBytes) {
    XPathKey k{};
    uint16_t len = 0;
    uint16_t endLen = 0;
    readXPathKey(f, k, len, endLen);
    if (!xpathLensValid(f, len, endLen)) break;  // truncated/corrupt
    if (xpathKeyHits(k, bm)) {
      out.resize(len);
      if (len) f.read(&out[0], len);
      if (outEnd) {
        outEnd->resize(endLen);
        if (endLen) f.read(&(*outEnd)[0], endLen);
      } else if (endLen) {
        f.seekCur(static_cast<int64_t>(endLen));
      }
      found = true;  // keep scanning — a later append for the same key supersedes this one
    } else if (len || endLen) {
      f.seekCur(static_cast<int64_t>(len) + static_cast<int64_t>(endLen));
    }
  }
  return found;
}

size_t BookmarkStore::whichHaveXPaths(bool* outHas, const size_t count) const {
  if (!outHas || count == 0) return 0;
  for (size_t i = 0; i < count; i++) outHas[i] = false;
  if (xpathFilePath.empty() || !Storage.exists(xpathFilePath.c_str())) return 0;
  HalFile f;
  if (!Storage.openFileForRead("BKS", xpathFilePath, f)) return 0;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != XPATH_VERSION) return 0;

  const size_t n = std::min(count, bookmarks.size());
  size_t found = 0;
  while (f.available() >= kXPathHeaderBytes) {
    XPathKey k{};
    uint16_t len = 0;
    uint16_t endLen = 0;
    readXPathKey(f, k, len, endLen);
    if (!xpathLensValid(f, len, endLen)) break;                                              // truncated/corrupt
    if (len || endLen) f.seekCur(static_cast<int64_t>(len) + static_cast<int64_t>(endLen));  // anchors not needed here
    for (size_t i = 0; i < n; i++) {
      if (outHas[i] || !xpathKeyHits(k, bookmarks[i])) continue;
      outHas[i] = true;  // a key identifies one spot, so no other bookmark can also match
      found++;
      break;
    }
  }
  return found;
}

size_t BookmarkStore::indexXPaths(uint32_t* outOffsets, const size_t count) const {
  if (!outOffsets || count == 0) return 0;
  for (size_t i = 0; i < count; i++) outOffsets[i] = 0;
  if (xpathFilePath.empty() || !Storage.exists(xpathFilePath.c_str())) return 0;
  HalFile f;
  if (!Storage.openFileForRead("BKS", xpathFilePath, f)) return 0;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != XPATH_VERSION) return 0;

  const size_t n = std::min(count, bookmarks.size());
  uint32_t at = static_cast<uint32_t>(sizeof(uint8_t));  // byte 0 is the version, so 0 is a safe "none"
  while (f.available() >= kXPathHeaderBytes) {
    XPathKey k{};
    uint16_t len = 0;
    uint16_t endLen = 0;
    readXPathKey(f, k, len, endLen);
    if (!xpathLensValid(f, len, endLen)) break;  // truncated/corrupt
    if (len || endLen) f.seekCur(static_cast<int64_t>(len) + static_cast<int64_t>(endLen));
    for (size_t i = 0; i < n; i++) {
      if (!xpathKeyHits(k, bookmarks[i])) continue;
      outOffsets[i] = at;  // a later append for the same key supersedes this one
      break;               // a key identifies one spot, so no other bookmark can also match
    }
    at += static_cast<uint32_t>(kXPathHeaderBytes) + len + endLen;
  }

  size_t found = 0;
  for (size_t i = 0; i < n; i++) {
    if (outOffsets[i] != 0) found++;
  }
  return found;
}

bool BookmarkStore::readXPathAt(const uint32_t offset, std::string& out, std::string& outEnd) const {
  out.clear();
  outEnd.clear();
  if (offset == 0 || xpathFilePath.empty()) return false;
  HalFile f;
  if (!Storage.openFileForRead("BKS", xpathFilePath, f)) return false;
  if (!f.seekSet(offset)) return false;
  if (f.available() < kXPathHeaderBytes) return false;
  XPathKey k{};
  uint16_t len = 0;
  uint16_t endLen = 0;
  readXPathKey(f, k, len, endLen);
  if (!xpathLensValid(f, len, endLen) || len == 0) return false;
  out.resize(len);
  f.read(&out[0], len);
  if (endLen) {
    outEnd.resize(endLen);
    f.read(&outEnd[0], endLen);
  }
  return true;
}

void BookmarkStore::compactXPaths() const {
  if (xpathFilePath.empty() || !Storage.exists(xpathFilePath.c_str())) return;

  if (bookmarks.empty()) {
    Storage.remove(xpathFilePath.c_str());  // nothing left to anchor — drop the sidecar
    return;
  }

  // Both paths of one record, back to back — the copy is verbatim, so it never needs
  // more than a single record's payload resident.
  auto buf = makeUniqueNoThrow<uint8_t[]>(2 * BOOKMARK_XPATH_MAX);
  if (!buf) {
    LOG_ERR("BKS", "OOM: %u bytes for xpath compaction", static_cast<unsigned>(2 * BOOKMARK_XPATH_MAX));
    return;
  }

  const std::string tmpPath = xpathFilePath + ".tmp";
  bool ok = false;
  {
    HalFile in;
    if (!Storage.openFileForRead("BKS", xpathFilePath, in)) return;
    uint8_t version = 0;
    serialization::readPod(in, version);
    if (version != XPATH_VERSION) return;

    HalFile out;
    if (!Storage.openFileForWrite("BKS", tmpPath, out)) return;
    serialization::writePod(out, XPATH_VERSION);

    while (in.available() >= kXPathHeaderBytes) {
      XPathKey k{};
      uint16_t len = 0;
      uint16_t endLen = 0;
      readXPathKey(in, k, len, endLen);
      if (!xpathLensValid(in, len, endLen)) break;  // truncated/corrupt
      if (len) in.read(buf.get(), len);
      if (endLen) in.read(buf.get() + len, endLen);
      const bool live =
          std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) { return xpathKeyHits(k, b); });
      if (live) {
        writeXPathKey(out, k, len, endLen);
        if (len) out.write(buf.get(), len);
        if (endLen) out.write(buf.get() + len, endLen);
      }
    }
    in.close();  // must close both before remove/rename on the same paths
    out.close();
    ok = true;
  }
  if (!ok) return;
  Storage.remove(xpathFilePath.c_str());
  Storage.rename(tmpPath.c_str(), xpathFilePath.c_str());
}

void BookmarkStore::exportTxt() const {
  if (bookFilePath.empty()) return;

  // Output filename: book basename, extension stripped, FAT-illegal chars sanitized.
  std::string name = bookFilePath;
  if (const auto slash = name.find_last_of('/'); slash != std::string::npos) name = name.substr(slash + 1);
  if (const auto dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  for (char& c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      c = '_';
    }
  }
  const std::string outPath = std::string(HIGHLIGHTS_DIR) + "/" + name + ".txt";

  // Quotes ("highlights") only — page bookmarks are not highlights, and excluding them
  // keeps the frequent hold-left bookmark toggle off this SD-write path. Order by book
  // position without copying Bookmark records.
  std::vector<size_t> order;
  for (size_t i = 0; i < bookmarks.size(); i++) {
    if (bookmarks[i].quote) order.push_back(i);
  }
  if (order.empty()) {
    if (Storage.exists(outPath.c_str())) Storage.remove(outPath.c_str());
    return;
  }
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (bookmarks[a].spineIndex != bookmarks[b].spineIndex) return bookmarks[a].spineIndex < bookmarks[b].spineIndex;
    return bookmarks[a].progress < bookmarks[b].progress;
  });

  Storage.mkdir(HIGHLIGHTS_DIR);
  HalFile out;
  if (!Storage.openFileForWrite("BKS", outPath, out)) {
    LOG_ERR("BKS", "Highlight export: open failed for %s", outPath.c_str());  // best-effort, never blocks the caller
    return;
  }

  const auto writeStr = [&out](const char* s) {
    if (s && *s) out.write(s, strlen(s));
  };

  char headerBuf[160];
  snprintf(headerBuf, sizeof(headerBuf), "# %s\n", bookTitle.c_str());
  writeStr(headerBuf);
  if (!bookAuthor.empty()) {
    snprintf(headerBuf, sizeof(headerBuf), "# by %s\n", bookAuthor.c_str());
    writeStr(headerBuf);
  }
  snprintf(headerBuf, sizeof(headerBuf), "# %u highlight%s\n\n", static_cast<unsigned>(order.size()),
           order.size() == 1 ? "" : "s");
  writeStr(headerBuf);

  std::string preview;  // reused; only one full preview resident at a time
  for (size_t idx : order) {
    const Bookmark& bm = bookmarks[idx];
    const char* chap = bm.chapterTitle[0] != '\0' ? bm.chapterTitle : "(unknown chapter)";
    snprintf(headerBuf, sizeof(headerBuf), "[%s, %d%%]\n", chap, static_cast<int>(std::lround(bm.progress * 100.0)));
    writeStr(headerBuf);
    if (readPreviewForKey(bm.spineIndex, bm.startWord, bm.endWord, preview) && !preview.empty()) {
      out.write(preview.data(), preview.size());
    } else {
      writeStr(bm.snippet[0] ? bm.snippet : "(quote)");  // synced-in quote: only the teaser is local
    }
    writeStr("\n\n");
  }
  LOG_DBG("BKS", "Highlight export: wrote %u highlight(s) to %s", static_cast<unsigned>(order.size()), outPath.c_str());
}
