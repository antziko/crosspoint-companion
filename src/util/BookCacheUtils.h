#pragma once

#include <KOReaderDocumentId.h>  // stripDeviceTag (pure, <string> only)

#include <string>

// Returns the untagged sibling path for a device-tagged book path (e.g.
// "/calibre/book (X3).epub" -> "/calibre/book.epub"), or "" if the name carries
// no auto-epub-optimizer device tag. Pure — no filesystem access; reuses the same
// tag normalization as KOReader filename sync. Host-testable.
inline std::string siblingOriginPath(const std::string& bookPath) {
  const size_t slash = bookPath.rfind('/');
  const std::string dir = slash == std::string::npos ? std::string() : bookPath.substr(0, slash + 1);
  const std::string name = slash == std::string::npos ? bookPath : bookPath.substr(slash + 1);
  const std::string origin = KOReaderDocumentId::stripDeviceTag(name);
  return origin == name ? std::string() : dir + origin;  // unchanged => this IS the origin
}

// On the first cache create of a device-tagged book (e.g. "book (X3).epub"),
// seeds its fresh cache with the reading stats + dated history (heatmap/timeline)
// from the untagged sibling ("book.epub") in the same folder — if that sibling
// exists and has stats, and this book has none yet. One-time, local, no network.
// No-op when: the name has no device tag, the sibling or its cache is absent, or
// this book already has stats. Progress is NOT copied (left to KOReader sync).
// Call after the cache dir is created at reader open.
// Returns true if anything was imported (history and/or stats) — the caller uses
// this to also prompt a KOReader sync so the freshly seeded book reconciles
// progress with the server.
bool importSiblingStatsIfNew(const std::string& bookPath, const std::string& cachePath);

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);

// Re-keys a book's bookmark sidecar files when it moves/renames, deriving the book type
// from the source extension (EPUB, XTC, or TXT). Does nothing for other file types.
// Use on any path that renames/moves a book file on the SD card.
void relocateBookBookmarks(const std::string& srcPath, const std::string& dstPath);

// Re-keys ALL sidecar state when a book file moves/renames: bookmark/tombstone files,
// the path-hash-keyed cache directory (progress, reading stats, rendered sections,
// cover), the EPUB cache label file, the recent-books entry, and the global resume
// pointer. Without this a moved book loses its progress and stats. Does nothing for
// non-book files. Call AFTER the file rename succeeded.
void relocateBookSidecars(const std::string& srcPath, const std::string& dstPath);

// Ensures cachePath/content_id.bin exists, recording the book's content fingerprint
// (KOReader partial-MD5) and its current path. tryRecoverBookCache() uses it to re-key
// the cache when the book is moved outside the firmware (e.g. on a PC). Cheap no-op
// when the file already exists. Call after the cache dir is created at reader open.
void ensureCacheContentId(const std::string& bookPath, const std::string& cachePath);

// If bookPath has no cache dir yet, scans /.crosspoint for an orphaned cache dir whose
// content_id matches this file's content (book moved/renamed outside the firmware) and
// relocates all sidecar state to the new path via relocateBookSidecars(). Returns true
// if a cache was recovered. Call BEFORE setupCacheDir() at reader open — an empty dir
// created first would mask the recovery condition.
bool tryRecoverBookCache(const std::string& bookPath);
