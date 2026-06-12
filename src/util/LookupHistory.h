#pragma once

#include <string>
#include <vector>

// Per-book lookup history. Stored as <cachePath>/dictionary_history.txt.
// Format: one entry per line, "word|STATUS\n" where STATUS is a single char.
// Oldest entry at top; newest at bottom. Deduplicated: re-looking up a word
// removes its previous entry and re-appends it as newest (refreshing status).
//
// All mutators stream the file line-by-line through a fixed stack buffer and
// never materialize the history in RAM. This code runs while the EPUB reader
// is still resident (the dictionary activities are pushed on top of it) where
// the largest free block can be a few KB; with -fno-exceptions a failed
// std::vector/std::string growth there aborts the firmware.
class LookupHistory {
 public:
  enum class Status { Direct = 'D', Stem = 'T', AltForm = 'Y', Suggestion = 'S', NotFound = 'X' };

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

  // Get the word at a 0-based newest-first index (0 = most recent) without
  // materializing the history. Returns "" if out of range.
  static std::string getWordNewestFirst(const std::string& cachePath, int index);

  // Remove entry at 0-based file index (oldest=0). Rewrites file without that entry.
  static bool removeAt(const std::string& cachePath, int index);

 private:
  static std::string filePath(const std::string& cachePath);
  static std::string tmpFilePath(const std::string& cachePath);
  // Stream each line of `path` through `fn` (fixed 256-byte line buffer, no
  // heap). `fn` returns false to stop early. Returns false iff the file could
  // not be opened.
  static bool forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len), void* ctx);
};
