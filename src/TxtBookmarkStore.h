#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Lightweight per-book bookmark store for the plain-text (.txt) reader.
//
// Unlike the EPUB BookmarkStore (which is keyed on spine/chapter/paragraph and
// participates in KOReader sync), a .txt file has no structure beyond a byte
// stream paginated for the current viewport. A TXT bookmark is therefore just a
// page index plus a short snippet for display. Page indices are advisory: they
// are recomputed against the current pagination, so they go stale if the index
// is rebuilt for a different viewport/font. The reader clamps on load.
//
// Persisted to "<cachePath>/txt_bookmarks.bin". No sync, no tombstones.

inline constexpr size_t TXT_BOOKMARK_SNIPPET_MAX = 64;

struct TxtBookmark {
  uint32_t page = 0;                            // 0-based page index
  char snippet[TXT_BOOKMARK_SNIPPET_MAX] = {};  // NUL-terminated display text
};

namespace TxtBookmarkStore {

// Read all bookmarks for the book at `cachePath`. Returns an empty vector when
// the file is missing, corrupt, or of an unknown version.
std::vector<TxtBookmark> load(const std::string& cachePath);

// Overwrite the bookmark file with `bookmarks`. Returns true on success.
bool save(const std::string& cachePath, const std::vector<TxtBookmark>& bookmarks);

enum class ToggleResult { Added, Removed, Error };

// Add a bookmark for `page` (with `snippet`) if none exists there, otherwise
// remove the existing one. The list stays sorted ascending by page. Persists
// immediately.
ToggleResult toggle(const std::string& cachePath, uint32_t page, const char* snippet);

// True if a bookmark exists for `page`.
bool hasBookmarkForPage(const std::string& cachePath, uint32_t page);

}  // namespace TxtBookmarkStore
