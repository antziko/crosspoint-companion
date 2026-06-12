#pragma once

#include <string>

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
