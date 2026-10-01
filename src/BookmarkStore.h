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

// KOReader XPath cap, stored on disk in the parallel .xpath file. Like QUOTE_PREVIEW_MAX
// this is NOT held resident per-record — 128 records would be ~25KB, and the Home screen
// already runs at ~23KB free. Deep ancestry ("/body/DocFragment[12]/body/div[2]/section[1]
// /p[147]/text()[2].233") runs ~70 bytes, so this leaves generous headroom.
inline constexpr size_t BOOKMARK_XPATH_MAX = 200;

// Combined cap on point bookmarks + quotes per book. The resident vector is one
// contiguous heap block (count * sizeof(Bookmark)); on the ESP32-C3 the max-contiguous
// allocation is well under what 1024 records would need, so the cap is set to a size the
// device can actually hold and load (128 * ~144 B ≈ 18 KB). Loads bound their reserve to
// this (readFromFile) so an over-large/corrupt file truncates gracefully instead of
// aborting on a failed allocation. Sync also reads it, to detect a remote set holding
// more records than this device can represent.
inline constexpr uint16_t MAX_BOOKMARKS = 128;

// Identity range reserved for a mark a KOReader peer made.
//
// A device keys a highlight by (spineIndex, startWord, endWord) and a point bookmark by
// (spineIndex, paragraphIndex). Both are this device's own coordinates: word indices are
// page-local, and the paragraph index comes from the section cache. A KOReader client can
// reproduce neither, so it hashes its XPointer into this band instead. An identity only
// has to be stable and unique, not derivable, and a mark keyed here round-trips through
// merge and storage like any other.
//
// A page holds a few hundred words and a chapter a few thousand paragraphs, so nothing
// this device produces reaches 0xC000. UINT16_MAX stays excluded: that means "no anchor".
inline constexpr uint16_t FOREIGN_KEY_BASE = 0xC000;

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
  // Exact visible-character offset of the anchor within its chapter, or 0 for "unknown".
  // The one coordinate that is both character-exact AND survives a re-layout: paragraphIndex
  // only names the paragraph, so a mark that a peer placed mid-paragraph lands on the page
  // the paragraph STARTS on, which on a smaller screen can be a page early. Set when a
  // peer's mark is adopted, where the resolver has already computed it. 4 bytes x 128 marks.
  uint32_t visibleTextOffset = 0;

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

  // ---- KOReader XPath sidecar ----
  // The anchor a KOReader client needs: CrossPoint's own (spineIndex, progress, word range)
  // is meaningless to crengine, so each bookmark additionally carries the XPath resolved
  // from its position. Stored out-of-line in .xpath, identity-keyed exactly as the merge key
  // is (see keyMatchFull), so it survives the bookmarks vector being re-sorted and costs no
  // resident RAM. Writes are appends; a later append for the same key supersedes earlier ones.
  //
  // Resolving an XPath re-streams the whole spine item, so callers generate these in a
  // batch (grouped by spine) at sync time, never per bookmark on the reading path.
  // `endXPath` is a highlight's end anchor: KOReader places a highlight from a pos0/pos1
  // pair, and CrossPoint's own endProgress is a copy of the start (addQuote), so the end
  // has to be resolved from the quoted text. Empty for a point bookmark.
  bool setXPath(const Bookmark& bm, const std::string& xpath, const std::string& endXPath = "") const;
  // Reads the XPath recorded for `bm`, or false (out cleared) when none is stored. `outEnd`,
  // when given, receives the end anchor (empty for a point bookmark).
  // Scans the whole sidecar per call, so do NOT loop it over the bookmark set to find which
  // marks still need anchoring — use whichHaveXPaths() for that.
  bool getXPath(const Bookmark& bm, std::string& out, std::string* outEnd = nullptr) const;

  // Where each loaded bookmark's anchor sits in the sidecar, in ONE pass. `outOffsets` is a
  // caller-owned array of `count` byte offsets indexed like getBookmarks(); 0 means the
  // bookmark has no anchor. Serialization uses this so it can fetch anchors with one seek
  // each instead of rescanning the whole file per bookmark. Returns how many are anchored.
  size_t indexXPaths(uint32_t* outOffsets, size_t count) const;

  // Read the anchor pair of the record at a byte offset from indexXPaths(). One seek and
  // one record, so it costs the same whatever the sidecar's size.
  bool readXPathAt(uint32_t offset, std::string& out, std::string& outEnd) const;

  // Which of the loaded bookmarks already carry an anchor, in ONE pass over the sidecar.
  // `outHas` is a caller-owned array of `count` bools, indexed like getBookmarks(); entries
  // past the bookmark count are set false. Payloads are skipped, not read. Returns how many
  // are anchored.
  size_t whichHaveXPaths(bool* outHas, size_t count) const;

  // Was this mark made by a KOReader peer? True for the whole life of a quote: its
  // synthetic word range IS its identity, so adoption cannot clear it.
  static bool isForeignMark(const Bookmark& bm);

  // A peer's mark that no device has placed yet, so its progress is still that peer's
  // guess rather than a page fraction of this pagination. A peer sends no page numbers
  // (cp/pc are 0 on the wire) and adoption is what fills them in, so the page count
  // doubles as the "placed here" flag -- and costs no I/O to read.
  static bool isUnplacedForeign(const Bookmark& bm) { return isForeignMark(bm) && bm.chapterPageCount == 0; }

  // How many marks isUnplacedForeign() holds for. Cheap: resident records only.
  size_t countUnplacedForeign() const;

  // This book's Lamport clock: the highest version any of its marks or tombstones
  // carries, rebuilt from file on load. It moves whenever a mark is added, deleted,
  // adopted or merged in, and not otherwise -- so an unchanged clock means retrying
  // work that failed last time would fail the same way.
  uint32_t clock() const { return lamportCounter; }

  // Index of the loaded mark with this identity, or SIZE_MAX. The identity is the merge
  // key, so this is how a caller re-finds a mark across an operation that re-sorts.
  size_t indexOfMark(bool quote, uint16_t spineIndex, uint16_t paragraphIndex, float progress, uint16_t startWord,
                     uint16_t endWord) const;

  // Re-file a foreign mark at the position its XPath actually resolves to. A peer can
  // send a correct anchor but only a guess at CrossPoint's own coordinates, so a mark
  // arrives keyed synthetically and positioned approximately; this corrects it once.
  //
  // A point bookmark is keyed by its paragraph anchor, so re-filing it is a different
  // spot: the synthetic one is tombstoned and a native one takes its place. Both name the
  // same XPath, so the peer re-keys its own annotation instead of gaining a second.
  //
  // A highlight is keyed by its word range, which cannot be derived without paginating
  // the chapter — not available where this runs. Its identity is left alone and only its
  // position corrected, which is what the page needs to find it; PageMarks anchors a
  // foreign highlight on its text rather than on the range.
  //
  // Sorts, so indices into getBookmarks() are invalidated. True when the store changed.
  //[[
  // Re-file a peer's mark under this device's coordinates.
  //
  // `progress` must be a page fraction (page / pageCount) like every other mark here, not
  // the character fraction an XPath resolves to -- see BookmarkAnchors::adoptForeign.
  // `page`/`pageCount` fill in the "page X of Y" the peer could not know; pass 0 for both
  // to leave whatever is recorded.
  //
  // `visibleTextOffset` is the anchor's exact character position in the chapter, and is
  // what the reader jumps by: a peer's page often begins mid-paragraph, and paragraphIndex
  // alone would then land on the page that paragraph STARTS on. Pass 0 for unknown.
  //
  // `endProgress` is the page fraction the quote ENDS at, and is what lets PageMarks draw a
  // highlight that crosses a page break -- a peer selects against its own, larger page, so
  // one selection there routinely spans two or three here. Pass a negative value to keep
  // the single-page behaviour (end == start), which is also what a point bookmark takes.
  //]]
  bool adoptForeignMark(size_t index, uint16_t spineIndex, float progress, uint16_t paragraphIndex,
                        const char* chapterTitle, const std::string& xpath, const std::string& endXPath = "",
                        uint16_t page = 0, uint16_t pageCount = 0, uint32_t visibleTextOffset = 0,
                        float endProgress = -1.0f);

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

  // The form the sync PUT sends: this book's own marks, each with the KOReader anchor
  // ("xp", plus "xp1" for a highlight's end) read from the sidecar. A mark with no stored
  // anchor is emitted exactly as serializeToJson would emit it, so a peer that predates
  // anchors still reads the blob.
  std::string serializeWithAnchors(size_t maxBodyBytes = SIZE_MAX) const;

  // Parse the sync blob into bookmarks + tombstones. Accepts both the object form
  // above and the legacy bare-array form (tombstones empty). Both outputs cleared
  // first. Returns false on malformed JSON.
  //
  // Anchors ("xp"/"xp1") are NOT returned here: the Bookmark record holds no strings, and
  // buffering a whole remote set of them would cost ~18KB at the point in sync where the
  // heap is tightest. They are not discarded, though — adoptRemoteXPaths() streams them
  // into the sidecar from the same blob, one at a time.
  //
  // outTruncated (optional) reports that the blob held more bookmarks or tombstones than
  // MAX_BOOKMARKS and the excess was dropped. A truncated parse must NOT be re-uploaded:
  // the PUT replaces the whole server blob, so the records this device could not hold
  // would be deleted for every peer. Callers skip the upload instead. Set false on entry.
  static bool parseFromJson(const char* json, std::vector<Bookmark>& outBms, std::vector<Tombstone>& outTombs,
                            bool* outTruncated = nullptr);

  // Take the anchors a remote blob carries, for spots this device has none for.
  //
  // parseFromJson cannot return them (see above), and re-deriving one costs a chapter
  // stream against an Epub the sync leg has deliberately released — so without this a
  // device that merges a peer's mark re-uploads it stripped of the anchor the peer had
  // already resolved, and every peer loses it until that device syncs again. Writing them
  // straight through to the sidecar as the blob is walked keeps one anchor pair resident
  // instead of the whole set.
  //
  // Only the sidecar path is needed, so this runs with no book loaded, at the post-WiFi
  // heap high-water before the local set is read. Static and path-keyed for that reason:
  // it must not depend on which book the singleton happens to hold.
  //
  // An anchor whose spot the following merge then rejects is left as an orphan record,
  // which reads already tolerate and compactXPaths drops at the next delete.
  //
  // @param json     the remote blob, exactly as parseFromJson takes it
  // @param filePath the book, keyed as loadForBook keys it
  // @return how many anchors were written
  static size_t adoptRemoteXPaths(const char* json, const std::string& filePath, const char* bookType);

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
  // Parallel full-preview file for quotes; see the .qtext block below for its layout.
  std::string qtextFilePath;
  // Parallel KOReader-XPath file; see the .xpath block below for its layout.
  std::string xpathFilePath;
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

  // Append one anchor record to a named sidecar. setXPath is this with the loaded book's
  // path; adoptRemoteXPaths needs it with no book loaded, so the path is a parameter.
  static bool appendXPathTo(const std::string& path, const Bookmark& bm, const std::string& xpath,
                            const std::string& endXPath);

  // Shared body of serializeToJson and serializeWithAnchors; see the .cpp for why the two
  // share one document rather than one wrapping the other.
  static std::string serializeInternal(const std::vector<Bookmark>& bms, const std::vector<Tombstone>& tombs,
                                       size_t maxBodyBytes, const BookmarkStore* anchorStore,
                                       const uint32_t* anchorOffsets);

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

  // Rewrite .xpath keeping only entries whose key still matches a live bookmark, copying
  // one entry at a time. Deletes the file when no bookmarks remain.
  void compactXPaths() const;

  // Delete a .xpath left by a different firmware version, before anything appends to it.
  void discardForeignXPathVersion() const;

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
