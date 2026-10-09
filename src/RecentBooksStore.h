#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

// Reading history, newest first. The list lives on SD (/.crosspoint/recent.tsv,
// one line per book, see RecentBooksLine.h); only its first HEAD_COUNT entries
// stay in RAM, for Home. The Recent Books screen reads the rest on demand
// through scan()/readAt(), so a long history costs no resident heap. Every
// change streams the file through a 1 KB line buffer into a replacement.
class RecentBooksStore {
 public:
  static constexpr int MAX_RECENT_BOOKS = 500;
  // At least every Home layout's book count (CoverGridHomeUi::MAX_BOOKS is 7).
  static constexpr int HEAD_COUNT = 10;

  static RecentBooksStore& getInstance() { return instance; }

  // Loads the head; converts a pre-history recent.json once.
  bool loadFromFile();

  // Move the book to the front (adding it if new), dropping entries past
  // MAX_RECENT_BOOKS. A book already at the front with the same details writes nothing.
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);
  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true if an entry was found and removed.
  bool removeByPath(const std::string& path);
  // Remove `path` and every entry under it as a folder (a deleted book or folder).
  // Returns true if anything was removed.
  bool removeUnder(const std::string& path);
  // Exchange the positions of two entries (manual reorder). False when either is missing.
  bool swapEntries(const std::string& pathA, const std::string& pathB);
  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Keeps the entry's list position.
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // The first HEAD_COUNT entries, newest first.
  const std::vector<RecentBook>& getBooks() const { return head; }

  // Every entry in order: fn(ctx, path, offset) per line, where offset is the
  // line's position for readAt(). Stop early by returning false. The path
  // view is valid only during the call.
  using ScanFn = bool (*)(void* ctx, std::string_view path, uint32_t offset);
  bool scan(ScanFn fn, void* ctx) const;
  // The entry whose line starts at `offset` (from scan()).
  bool readAt(uint32_t offset, RecentBook& out) const;

 private:
  RecentBooksStore() = default;
  static RecentBooksStore instance;

  std::vector<RecentBook> head;

  bool reloadHead();
  bool migrateJson();
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
