#pragma once

#include <string>
#include <vector>

// Per-book lookup history. Stored as <cachePath>/dictionary_history.txt.
// Format: one entry per line, "word|STATUS|VER\n" where STATUS is a single char
// and VER is a decimal Lamport version. Legacy lines ("word|STATUS" or bare
// "word") parse as VER=0. Oldest entry at top; newest at bottom. Deduplicated:
// re-looking up a word removes its previous entry and re-appends it as newest
// (refreshing status, stamping a fresh version).
//
// Cross-device sync (KOReaderSync "dh" blob) is Lamport-versioned to mirror the
// bookmark store: a deleted word leaves a tombstone in <cachePath>/
// dictionary_history.tomb ("word|VER\n"); on merge the higher version wins per
// word, so deletions propagate and a re-lookup after a delete correctly wins.
// The per-book Lamport clock lives in <cachePath>/dictionary_history.ver.
//
// All mutators stream the file line-by-line through a fixed stack buffer and
// never materialize the history in RAM. This code runs while the EPUB reader
// is still resident (the dictionary activities are pushed on top of it) where
// the largest free block can be a few KB; with -fno-exceptions a failed
// std::vector/std::string growth there aborts the firmware.
class LookupHistory {
 public:
  enum class Status { Direct = 'D', Stem = 'T', AltForm = 'Y', Suggestion = 'S', NotFound = 'X' };

  // Filename of the per-book history file inside a book's cache dir. Exposed so
  // sibling-seed code (BookCacheUtils) names the same file without a literal copy.
  static constexpr char FILE_NAME[] = "dictionary_history.txt";

  struct Entry {
    std::string word;
    Status status = Status::NotFound;
  };

  // Append word+status. Evicts oldest entries if over cap. Returns the new
  // entry count, or -1 on I/O failure (existing history left unchanged).
  static int addWord(const std::string& cachePath, const std::string& word, Status status);

  // Conditional addWord: short-circuits if disabled, word empty, or cachePath empty.
  // Single guarded entry point used by all dictionary lookup recording sites.
  static void addWordIf(const std::string& cachePath, const std::string& word, Status status, bool enabled);

  // Load all entries in most-recent-first order. The one API that materializes
  // the history (the history-list UI genuinely needs it); its callers run in
  // healthier-heap contexts than the in-reader lookup path.
  static std::vector<Entry> load(const std::string& cachePath);

  // Total entry count without materializing the history (one streaming pass).
  // The windowed history-list UI uses this for scroll bounds + the header total.
  static int count(const std::string& cachePath);

  // Fill `out[0..n)` with up to `n` entries in newest-first order starting at
  // newest-first index `startNewest` (0 = most recent). One streaming pass, no
  // heap beyond the captured Entry words. Returns the number actually filled
  // (fewer than `n` near the end, 0 if startNewest is past the end). Lets the
  // list UI keep only one on-screen window in RAM regardless of history size.
  static int loadWindow(const std::string& cachePath, int startNewest, int n, Entry* out);

  // Get the word at a 0-based newest-first index (0 = most recent) without
  // materializing the history. Returns "" if out of range.
  static std::string getWordNewestFirst(const std::string& cachePath, int index);

  // Get the full entry (word + status) at a 0-based newest-first index without
  // materializing the history. Returns false if out of range.
  static bool getEntryNewestFirst(const std::string& cachePath, int index, Entry& out);

  // Remove entry at 0-based file index (oldest=0). Rewrites file without that
  // entry and records a tombstone (delete propagates on sync).
  static bool removeAt(const std::string& cachePath, int index);

  // --- Cross-device sync (KOReaderSync "dh" blob) ---------------------------

  // Serialize tombstones + newest history entries into `out` (a self-describing
  // text blob: lines "T word|VER" then "H word|STATUS|VER"), packed up to `cap`
  // bytes. Tombstones first so deletions always propagate; remaining budget is
  // filled with the newest history entries. Returns bytes written (0 if empty).
  // If outHistCount / outTombCount are non-null, they receive the number of
  // history entries and tombstones written (the "mine" uploaded counts, after any
  // cap truncation).
  static size_t serializeBlob(const std::string& cachePath, uint8_t* out, size_t cap, int* outHistCount = nullptr,
                              int* outTombCount = nullptr);

  // Merge a peer device's serializeBlob() output into this book's history using
  // Lamport-version-wins per word: a remote add/tombstone is applied only if it is
  // newer than the word's local state (its history version, else tombstone version,
  // else nothing — so a brand-new word always merges). Raises the local Lamport
  // clock past every version seen. Returns the number of remote ADDS applied; if
  // outDeleted is non-null, receives the number of remote DELETES applied.
  static int mergeBlob(const std::string& cachePath, const uint8_t* blob, size_t len, int* outDeleted = nullptr);

 private:
  static std::string filePath(const std::string& cachePath);
  static std::string tmpFilePath(const std::string& cachePath);
  static std::string tombFilePath(const std::string& cachePath);
  static std::string verFilePath(const std::string& cachePath);

  // Per-book Lamport clock (persisted in dictionary_history.ver). loadCounter
  // returns the current clock, reconstructing it from the max version across
  // history + tombstones if the .ver file is missing (legacy upgrade). nextVersion
  // stamps a new local edit; observeVersion raises the clock past a remote one.
  static uint32_t loadCounter(const std::string& cachePath);
  static uint32_t nextVersion(const std::string& cachePath);
  static void observeVersion(const std::string& cachePath, uint32_t v);

  // Core add with an explicit version (dedup-move-to-newest, cap eviction,
  // clears any tombstone for `word`). addWord wraps this with nextVersion().
  static int addWordVer(const std::string& cachePath, const std::string& word, Status status, uint32_t version);

  // Word's local history version, or -1 if the word is not in the history file.
  // (Distinguishing "absent" from "present at version 0" is required so legacy
  // v0 entries from a peer still merge into a device that lacks the word.)
  static int historyVersionOf(const std::string& cachePath, const std::string& word);
  // Word's tombstone version, or 0 if not tombstoned.
  static uint32_t tombstoneVersionOf(const std::string& cachePath, const std::string& word);

  // Tombstone helpers (dictionary_history.tomb).
  static void setTombstone(const std::string& cachePath, const std::string& word, uint32_t version);
  static void clearTombstone(const std::string& cachePath, const std::string& word);
  // Rewrite the history file without `word` (used by tombstone application).
  static void removeWord(const std::string& cachePath, const std::string& word);
  // Stream each line of `path` through `fn` (fixed 256-byte line buffer, no
  // heap). `fn` returns false to stop early. Returns false iff the file could
  // not be opened.
  static bool forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len), void* ctx);
};
