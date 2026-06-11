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
