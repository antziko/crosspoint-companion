#pragma once
#include <cstddef>
#include <cstdint>
#include <stdint.h>
#include <string>
#include <vector>

// chapterTitle is always NUL-terminated within BOOKMARK_CHAPTER_TITLE_MAX bytes.
// This size is part of the on-disk format — do not change without incrementing the file version.
inline constexpr size_t BOOKMARK_CHAPTER_TITLE_MAX = 48;
inline constexpr size_t BOOKMARK_SNIPPET_MAX = 64;

struct Bookmark {
  uint16_t spineIndex;
  float progress;
  // Lamport version: a per-book logical clock bumped on every local add/delete, used
  // to order create-vs-delete across devices without a real clock (ESP32-C3 has no
  // battery-backed RTC). Higher version wins on merge. Occupies the slot that used to
  // hold an always-zero "timestamp", so the on-disk bookmark format is unchanged —
  // pre-existing bookmarks read back as version 0 (lowest priority).
  uint32_t version;
  char chapterTitle[BOOKMARK_CHAPTER_TITLE_MAX];
  // Optional 1-based paragraph anchor from the section cache. UINT16_MAX means unavailable.
  uint16_t paragraphIndex = UINT16_MAX;
  char snippet[BOOKMARK_SNIPPET_MAX] = {};
  // Display-only snapshot of the page position within the chapter at bookmark time, used to
  // show "page X/Y" in the bookmark list. chapterCurrentPage is 0-based (display adds 1).
  // Both are a snapshot: they go stale if render settings change the chapter's pagination, so
  // they are advisory. 0 page count means "unknown" (legacy bookmark or a peer that didn't
  // send it) and the list falls back to showing just the percentage + chapter title.
  uint16_t chapterCurrentPage = 0;
  uint16_t chapterPageCount = 0;
  // Session-only flag: true for the "return here" bookmark auto-dropped when the user
  // jumps to another chapter, so the reader can show a distinct icon for it. Deliberately
  // NOT serialized to the bookmark file and NOT synced — it is a within-session navigation
  // aid, so it costs no on-disk format change and reverts to a plain bookmark on reload.
  bool returnMark = false;
};

// Marks a bookmark that was deleted, so sync removes it from the server and other
// devices. Identity mirrors the merge key: (spineIndex, paragraphIndex) when an anchor
// exists, else (spineIndex, progress). `version` is the Lamport stamp at deletion time;
// merge keeps whichever of {bookmark, tombstone} for a spot has the higher version.
struct Tombstone {
  uint16_t spineIndex;
  uint16_t paragraphIndex;  // UINT16_MAX if no anchor
  float progress;
  uint32_t version = 0;
};

struct BookmarkedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count;
};

class BookmarkStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
  };

  static BookmarkStore& getInstance() { return instance; }

  // Load bookmarks for a book. Returns true even when no file exists yet (empty store).
  // bookType must be "epub", "xtc", or "txt" — used to form the cache filename.
  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addBookmark(uint16_t spineIndex, float progress, int pageCount, const char* chapterTitle,
                        uint16_t paragraphIndex = UINT16_MAX, const char* snippet = nullptr,
                        bool returnMark = false, int currentPage = 0);
  void removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool removeBookmarkAt(size_t index);
  // Consume the session "return here" mark at this spot (matched like the merge key):
  // removes it only when the matching bookmark is a return mark. Used when the user
  // reopens it to navigate back — the one-shot aid has served its purpose. No-op (returns
  // false) for a normal bookmark. Tombstones the delete so it also propagates on sync.
  bool removeReturnMarkAt(uint16_t spineIndex, uint16_t paragraphIndex, float progress);
  bool hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  // True when the bookmark covering this page is the session "return here" mark.
  bool isReturnMarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  const std::vector<Bookmark>& getBookmarks() const { return bookmarks; }

  // ---- KOReader bookmark sync (self-hosted server extension) ----

  const std::vector<Tombstone>& getTombstones() const { return tombstones; }

  // Serialize bookmarks + tombstones to the sync blob:
  //   {"bookmarks":[...],"tombstones":[...]}
  static std::string serializeToJson(const std::vector<Bookmark>& bms, const std::vector<Tombstone>& tombs);

  // Parse the sync blob into bookmarks + tombstones. Accepts both the object form
  // above and the legacy bare-array form (tombstones empty). Both outputs cleared
  // first. Returns false on malformed JSON.
  static bool parseFromJson(const char* json, std::vector<Bookmark>& outBms, std::vector<Tombstone>& outTombs);

  // Reconcile remote state into the currently loaded book and persist. Last-writer-
  // wins by Lamport version: for each spot, the bookmark or tombstone with the higher
  // version wins, so a delete done after seeing a bookmark propagates, and a re-add
  // done after a delete resurrects — both converge without a wall clock. The local
  // counter is advanced past every version seen so future local edits outrank them.
  // Self-persists both the bookmark and tombstone files. Returns bookmarks added.
  size_t mergeFrom(const std::vector<Bookmark>& remoteBookmarks, const std::vector<Tombstone>& remoteTombstones);

  // Flush to disk if dirty. Called automatically by add/remove; also call from reader onExit().
  void saveToFile();

  // Remove all bookmarks for the current book and delete its bookmark file.
  void clearAll();

  // Returns true if any bookmark files exist on disk (directory scan, no file parsing).
  static bool hasAnyBookmarks();

  // Delete the bookmark file for a given file path and book type without loading the book.
  // bookType must be "epub", "xtc", or "txt".
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);

  // Scan /.crosspoint/bookmarks/ and populate `out` with one entry per book that has bookmarks.
  // Reads only the file header (does not load full bookmark records).
  // Caller should reserve `out` before calling.
  static bool getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out);

 private:
  static BookmarkStore instance;

  std::vector<Bookmark> bookmarks;
  std::vector<Tombstone> tombstones;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  std::string tombFilePath;
  bool dirty = false;
  bool tombDirty = false;

  // Per-book Lamport clock. Reconstructed on load as the max version across all
  // bookmarks and tombstones (the highest-version entry always survives a merge, so
  // this lower bound is exact). nextVersion() stamps a new local edit; observeVersion()
  // raises it past versions seen from a remote during merge.
  uint32_t lamportCounter = 0;
  uint32_t nextVersion() { return ++lamportCounter; }
  void observeVersion(uint32_t v) {
    if (v > lamportCounter) lamportCounter = v;
  }

  bool readFromFile();
  bool writeToFile() const;

  // Keep `bookmarks` ordered by section (spineIndex) then position (progress).
  // The list view deletes by vector index, so this order is what the user sees.
  void sortBookmarks();

  // Tombstone persistence (separate .tomb file; bookmark format untouched).
  bool readTombstones();
  bool writeTombstones() const;
  void saveTombstones();          // flush if dirty; deletes file when empty
  void addTombstone(const Bookmark& bm);       // record a delete (dedup by key)
  void clearTombstoneFor(const Bookmark& bm);  // un-delete on re-add
};

#define BOOKMARKS BookmarkStore::getInstance()
