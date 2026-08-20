#pragma once

#include <KOReaderDocumentId.h>  // stripDeviceTag / swapAuthorTitle (pure, <string> only)

#include <string>
#include <vector>

// Returns candidate untagged sibling paths for a device-tagged book path, or an
// empty list if the name carries no auto-epub-optimizer device tag. Candidates,
// in priority order:
//   1. the tag-stripped name, same author/title order
//      ("/calibre/A - B (X3).epub" -> "/calibre/A - B.epub")
//   2. the tag-stripped name with author/title order literally reversed, when the
//      name has exactly one " - " ("/calibre/A - B (X3).epub" -> "/calibre/B - A.epub")
// Candidate 2 is the literal reverse (not the sorted canonical form): the on-disk
// sibling could be in either order, so we must name the opposite ordering exactly.
// The same " - " / exactly-one-separator rule as KOReader filename-sync applies.
// Pure — no filesystem access. Host-testable. The caller picks the first that
// exists on disk.
inline std::vector<std::string> siblingOriginPaths(const std::string& bookPath) {
  std::vector<std::string> out;
  const size_t slash = bookPath.rfind('/');
  const std::string dir = slash == std::string::npos ? std::string() : bookPath.substr(0, slash + 1);
  const std::string name = slash == std::string::npos ? bookPath : bookPath.substr(slash + 1);
  const std::string untagged = KOReaderDocumentId::stripDeviceTag(name);
  if (untagged == name) {
    return out;  // no device tag => this IS the origin, nothing to seed
  }
  out.push_back(dir + untagged);  // (1) same-order untagged sibling

  // (2) literal author/title-reversed sibling, only on exactly one " - " separator.
  const size_t dot = untagged.rfind('.');
  const std::string stem = (dot == std::string::npos) ? untagged : untagged.substr(0, dot);
  const std::string ext = (dot == std::string::npos) ? std::string() : untagged.substr(dot);
  const size_t first = stem.find(" - ");
  if (first != std::string::npos && stem.find(" - ", first + 3) == std::string::npos) {
    const std::string a = stem.substr(0, first);
    const std::string b = stem.substr(first + 3);
    if (!a.empty() && !b.empty()) {
      out.push_back(dir + b + " - " + a + ext);  // reversed order
    }
  }
  return out;
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

// Tally returned by pruneOrphanCaches().
struct CachePruneResult {
  int removed = 0;  // orphan cache dirs deleted (book no longer on the card)
  int kept = 0;     // cache dirs whose recorded book still exists
  int skipped = 0;  // dirs left untouched: no/unreadable or corrupt content_id
  int failed = 0;   // orphans whose removeDir() failed
};

// Scans /.crosspoint and classifies each book cache dir WITHOUT deleting anything.
// Appends the dir names (e.g. "epub_12345") of orphan caches — those whose recorded
// content_id path no longer exists on the SD card — to orphanDirNames. Dirs without a
// readable content_id.bin, or whose recorded path does not hash back to the dir, are
// counted as skipped (cannot prove orphan). Returns the tally; res.removed is the count
// of orphans FOUND (not yet removed), res.failed is always 0. Use to preview before
// removeOrphanCaches(). Device-only (uses Storage).
CachePruneResult scanOrphanCaches(std::vector<std::string>& orphanDirNames);

// Removes the given cache dirs (names relative to /.crosspoint, as produced by
// scanOrphanCaches) and each one's sibling "<dir>--<title>-by-<author>.txt" label.
// Returns a tally with res.removed / res.failed populated. Device-only (uses Storage).
CachePruneResult removeOrphanCaches(const std::vector<std::string>& orphanDirNames);

// Convenience: scanOrphanCaches() then removeOrphanCaches() in one call (no preview).
// Selective alternative to clearing the whole cache: live books keep their progress/
// stats. Returns the combined tally. Device-only (uses Storage).
CachePruneResult pruneOrphanCaches();

// Deletes ONE book's cache dir (name relative to /.crosspoint, e.g. "epub_12345", as
// produced by scanOrphanCaches or the reading-stats Books tab) and its sibling
// "<dir>--<title>-by-<author>.txt" label. Unlike pruneOrphanCaches this makes no
// orphan check — the caller has chosen to clear a book that may still be on the card,
// so its rendered sections, cover, reading stats AND progress all go. Returns true if
// the dir was removed. Device-only (uses Storage).
bool removeBookCache(const std::string& dirName);

// Returns the book path recorded in a cache dir's content_id.bin (dirName relative to
// /.crosspoint, e.g. "epub_12345"), or an empty string when the dir has no readable
// fingerprint (caches written before content_id.bin existed) or the recorded path does
// not hash back to this dir. The dir name is a one-way hash of the path, so this file is
// the only way back from a cache dir to its book. Device-only (uses Storage).
std::string recordedBookPathForCache(const std::string& dirName);

// Removes the book state that lives OUTSIDE the path-hash cache dir: bookmark/tombstone
// sidecars, the recent-books entry, and the global resume pointer. Pair with
// removeBookCache() to leave a book as if it had never been opened. Does nothing for
// non-book files. No-op per item when nothing references bookPath. Device-only.
void forgetBookSidecars(const std::string& bookPath);

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
