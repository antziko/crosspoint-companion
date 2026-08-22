#pragma once
#include <stdint.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// chapterTitle is always NUL-terminated within BOOKMARK_CHAPTER_TITLE_MAX bytes.
// This size is part of the on-disk format — do not change without incrementing the file version.
inline constexpr size_t BOOKMARK_CHAPTER_TITLE_MAX = 48;
inline constexpr size_t BOOKMARK_SNIPPET_MAX = 64;
// Quote ("highlight") full preview text cap, stored on disk in the parallel .qtext
// file and shown in QuoteViewerActivity. NOT held resident per-record (the in-RAM
// teaser is snippet[BOOKMARK_SNIPPET_MAX]) and NOT synced — only one preview is read
// into RAM at a time, so this can comfortably exceed the snippet length.
inline constexpr size_t QUOTE_PREVIEW_MAX = 512;

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

  // ---- Quote ("highlight") range (v8 additions) ----
  // A quote is a ranged mark capturing selected text; a point bookmark leaves these at
  // their defaults. All fixed-size PODs, so the record stays trivially copyable and the
  // verbatim relocate/serialize paths are unaffected. The full preview text lives in the
  // parallel .qtext file (see QUOTE_PREVIEW_MAX); snippet[] holds the resident teaser
  // used by the list and by sync. `quote` is stored explicitly (not inferred from the
  // range) so a single-word selection at word 0 is not mistaken for a point bookmark.
  bool quote = false;
  uint16_t endSpineIndex = 0;
  float endProgress = 0.0f;
  uint16_t startWord = 0;  // page-local word index of the selection start
  uint16_t endWord = 0;    // page-local word index of the selection end (inclusive)

  bool isQuote() const { return quote; }
};

// Marks a bookmark that was deleted, so sync removes it from the server and other
// devices. Identity mirrors the merge key: for a point bookmark, (spineIndex,
// paragraphIndex) when an anchor exists, else (spineIndex, progress); for a quote,
// (spineIndex, startWord, endWord). `version` is the Lamport stamp at deletion time;
// merge keeps whichever of {bookmark, tombstone} for a spot has the higher version.
struct Tombstone {
  uint16_t spineIndex;
  uint16_t paragraphIndex;  // UINT16_MAX if no anchor
  float progress;
  uint32_t version = 0;
  // Quote range identity (v3 tombstone additions). `quote` stored explicitly to match
  // Bookmark; startWord/endWord carry the deleted quote's range key.
  bool quote = false;
  uint16_t startWord = 0;
  uint16_t endWord = 0;
  bool isQuote() const { return quote; }
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
                        uint16_t paragraphIndex = UINT16_MAX, const char* snippet = nullptr, bool returnMark = false,
                        int currentPage = 0);

  // Add a ranged quote ("highlight"). The anchor (spineIndex, progress, chapterTitle,
  // page snapshot) matches how a point bookmark anchors; startWord/endWord are the
  // page-local selection extent. `preview` is the full selected text (truncated to
  // QUOTE_PREVIEW_MAX on disk); its first BOOKMARK_SNIPPET_MAX-1 chars are stored in
  // the resident snippet teaser used by the list and by sync. Shares the same
  // MAX_BOOKMARKS limit as point bookmarks (one combined budget per book).
  AddResult addQuote(uint16_t spineIndex, float progress, uint16_t startWord, uint16_t endWord, int pageCount,
                     const char* chapterTitle, const char* preview, int currentPage = 0);

  // Remove a quote identified by its range key (spineIndex, startWord, endWord). Writes
  // a tombstone so the delete propagates on sync. No-op (returns false) if not found.
  bool removeQuoteByRange(uint16_t spineIndex, uint16_t startWord, uint16_t endWord);

  // Read the full preview text for the bookmark at `index` (into the current sorted
  // vector) from the parallel .qtext file. Returns false (and clears `out`) for a point
  // bookmark, a missing/!desynced .qtext, or an out-of-range index. Only one preview is
  // held in RAM at a time — callers must not cache the whole set.
  bool readPreviewAt(size_t index, std::string& out) const;

  void removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool removeBookmarkAt(size_t index);
  // Consume the session "return here" mark at this spot (matched like the merge key):
  // removes it only when the matching bookmark is a return mark. Used when the user
  // reopens it to navigate back — the one-shot aid has served its purpose. No-op (returns
  // false) for a normal bookmark. Return marks are device-only, so no tombstone is written.
  bool removeReturnMarkAt(uint16_t spineIndex, uint16_t paragraphIndex, float progress);
  // Consume the session "return here" mark covering this page, whichever way the reader
  // arrived — a page turn back to it counts as having found the way back, same as
  // reopening it from the list. Returns true when one was removed.
  bool removeReturnMarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  // Page-level presence split by mark type, for the reader's status-bar indicators (a page
  // may hold both — show both icons). "Point" excludes quotes; "Quote" is quotes only.
  bool hasPointBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool hasQuoteForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  // True when the bookmark covering this page is the session "return here" mark.
  bool isReturnMarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  const std::vector<Bookmark>& getBookmarks() const { return bookmarks; }

  // ---- KOReader bookmark sync (self-hosted server extension) ----

  const std::vector<Tombstone>& getTombstones() const { return tombstones; }

  // Serialize bookmarks + tombstones to the sync blob:
  //   {"bookmarks":[...],"tombstones":[...]}
  // maxBodyBytes caps the serialized length the internal reserve() may request. The
  // sync PUT now runs inside a keep-alive session (the GET's mbedTLS arena is still
  // held, leaving little free heap), and out.reserve() OOM-aborts under -fno-exceptions;
  // the caller passes the live largest contiguous block so an oversized body returns
  // empty ("skip upload") instead of crashing. Default = no cap (host tests / callers
  // at full heap).
  static std::string serializeToJson(const std::vector<Bookmark>& bms, const std::vector<Tombstone>& tombs,
                                     size_t maxBodyBytes = SIZE_MAX);

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

  // Re-key the bookmark + tombstone files when a book moves/renames (filename is keyed by
  // crc32 of the path). Without this, a moved book loses its bookmarks. No-op if srcPath ==
  // dstPath or no files exist. bookType must be "epub", "xtc", or "txt".
  static void relocateForFilePath(const std::string& srcPath, const std::string& dstPath, const std::string& bookType);

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
  // Parallel full-preview file: one length-prefixed entry per bookmark, rewritten in
  // lockstep with the .bin in writeToFile() so positional reads (readPreviewAt) stay in
  // sync with the (sorted) bookmarks vector. Point bookmarks store a zero-length entry.
  std::string qtextFilePath;
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

  // ---- Full-preview (.qtext) store ----
  // Identity-keyed by (spineIndex, startWord, endWord), append-on-add, streamed on read
  // (one entry in RAM at a time), and stream-compacted on delete/merge so no path ever
  // holds all previews resident. Decoupled from the .bin's sorted order, so reordering
  // the bookmarks vector never desyncs it.
  bool appendPreview(uint16_t spineIndex, uint16_t startWord, uint16_t endWord, const std::string& text) const;
  bool readPreviewForKey(uint16_t spineIndex, uint16_t startWord, uint16_t endWord, std::string& out) const;
  // Rewrite .qtext keeping only entries whose key is still a live quote in `bookmarks`,
  // copying one entry at a time. Deletes the file when no quotes remain.
  void compactPreviews() const;

  void exportTxt() const;  // best-effort dump of marks to /highlights/<book>.txt

  // Keep `bookmarks` ordered by section (spineIndex) then position (progress).
  // The list view deletes by vector index, so this order is what the user sees.
  void sortBookmarks();

  // Tombstone persistence (separate .tomb file; bookmark format untouched).
  bool readTombstones();
  bool writeTombstones() const;
  void saveTombstones();                       // flush if dirty; deletes file when empty
  void addTombstone(const Bookmark& bm);       // record a delete (dedup by key)
  void clearTombstoneFor(const Bookmark& bm);  // un-delete on re-add
};

#define BOOKMARKS BookmarkStore::getInstance()
