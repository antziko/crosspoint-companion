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
  uint32_t timestamp;
  char chapterTitle[BOOKMARK_CHAPTER_TITLE_MAX];
  // Optional 1-based paragraph anchor from the section cache. UINT16_MAX means unavailable.
  uint16_t paragraphIndex = UINT16_MAX;
  char snippet[BOOKMARK_SNIPPET_MAX] = {};
};

// Marks a bookmark that was deleted locally, so sync removes it from the server and
// other devices instead of resurrecting it via the additive merge. Identity mirrors the
// merge key: (spineIndex, paragraphIndex) when an anchor exists, else (spineIndex, progress).
struct Tombstone {
  uint16_t spineIndex;
  uint16_t paragraphIndex;  // UINT16_MAX if no anchor
  float progress;
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
                        uint16_t paragraphIndex = UINT16_MAX, const char* snippet = nullptr);
  void removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool removeBookmarkAt(size_t index);
  bool hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
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

  // Reconcile remote state into the currently loaded book and persist:
  //   1. union remote tombstones into local,
  //   2. drop any local bookmark hit by a tombstone (propagates deletes),
  //   3. additively add remote bookmarks not already present and not tombstoned.
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
