#pragma once

#include <cstddef>
#include <cstdint>
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

  // Outcome of a serializeBlob() pass. histCount/tombCount are the entries written
  // (the "mine" uploaded counts, after any cap truncation); maxVer is the highest
  // Lamport version emitted across both tombstones and history (0 if nothing was
  // written); truncated is true if any in-range line was dropped to fit `cap`.
  struct BlobStats {
    int histCount = 0;
    int tombCount = 0;
    uint32_t maxVer = 0;
    bool truncated = false;
  };

  // Serialize tombstones + newest history entries into `out` (a self-describing
  // text blob: lines "T word|VER" then "H word|STATUS|VER"), packed up to `cap`
  // bytes. Tombstones first so deletions always propagate; remaining budget is
  // filled with the newest history entries. With sinceVersion > 0 this emits a
  // DELTA: only lines whose Lamport version exceeds sinceVersion (used by
  // serializeForUpload to ship just what changed since the last confirmed upload).
  // Returns bytes written (0 if empty). If outStats is non-null it receives the
  // counts, max version, and truncation flag.
  static size_t serializeBlob(const std::string& cachePath, uint8_t* out, size_t cap, uint32_t sinceVersion = 0,
                              BlobStats* outStats = nullptr);

  // Pick a delta or keyframe blob for upload and serialize it into `out`.
  //
  // A keyframe is the full state (sinceVersion = 0); a delta is only the lines
  // newer than the last confirmed-uploaded version (the watermark in
  // dictionary_history.sync). A keyframe is forced when: the watermark is unset
  // (first upload), the watermark is ahead of the local Lamport clock (the clock
  // regressed — e.g. cache rebuilt), KEYFRAME_EVERY deltas have elapsed since the
  // last keyframe, or a delta would truncate (in which case the full state is sent
  // instead so no in-range line is silently dropped). Returns bytes written;
  // outStats / outIsKeyframe receive the pass result. Does NOT advance the
  // watermark — call commitUpload() only after the PUT is confirmed.
  static size_t serializeForUpload(const std::string& cachePath, uint8_t* out, size_t cap,
                                   BlobStats* outStats = nullptr, bool* outIsKeyframe = nullptr);

  // Advance the upload watermark after a confirmed-successful PUT of the blob
  // described by `uploaded`. Sets the watermark to the highest version uploaded
  // (never regresses it) and either resets the keyframe counter (wasKeyframe) or
  // increments it. MUST be called only on PUT success, so a failed upload re-sends
  // the same range next time.
  static void commitUpload(const std::string& cachePath, const BlobStats& uploaded, bool wasKeyframe);

  // Number of delta uploads between forced keyframes. A keyframe re-sends the full
  // state, healing any line a prior delta dropped to a cap (bounded staleness).
  static constexpr uint32_t KEYFRAME_EVERY = 8;

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
  static std::string syncFilePath(const std::string& cachePath);

  // Upload watermark, persisted in dictionary_history.sync as two decimal numbers
  // "lastVer uploadsSinceKeyframe". lastVer is the highest Lamport version known to
  // have been accepted by the server; deltas serialize only versions above it.
  struct SyncWatermark {
    uint32_t lastVer = 0;
    uint32_t uploadsSinceKeyframe = 0;
  };
  static SyncWatermark loadWatermark(const std::string& cachePath);
  static void storeWatermark(const std::string& cachePath, const SyncWatermark& wm);

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
