#include "TxtBookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t TXT_BM_MAGIC = 0x5458424D;  // "TXBM"
constexpr uint8_t TXT_BM_VERSION = 1;
constexpr uint32_t TXT_BM_MAX_ENTRIES = 256;  // sanity cap to reject corrupt files

std::string bookmarksPath(const std::string& cachePath) { return cachePath + "/txt_bookmarks.bin"; }
}  // namespace

std::vector<TxtBookmark> TxtBookmarkStore::load(const std::string& cachePath) {
  std::vector<TxtBookmark> out;

  HalFile f;
  if (!Storage.openFileForRead("TBM", bookmarksPath(cachePath), f)) {
    return out;
  }

  uint32_t magic = 0;
  serialization::readPod(f, magic);
  if (magic != TXT_BM_MAGIC) {
    LOG_DBG("TBM", "Bad bookmark magic, ignoring");
    return out;
  }

  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != TXT_BM_VERSION) {
    LOG_DBG("TBM", "Unknown bookmark version %u, ignoring", version);
    return out;
  }

  uint32_t count = 0;
  serialization::readPod(f, count);
  if (count > TXT_BM_MAX_ENTRIES) {
    LOG_ERR("TBM", "Bookmark count %u exceeds cap, ignoring", count);
    return out;
  }

  out.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    TxtBookmark bm;
    serialization::readPod(f, bm.page);
    if (f.read(bm.snippet, TXT_BOOKMARK_SNIPPET_MAX) != static_cast<int>(TXT_BOOKMARK_SNIPPET_MAX)) {
      LOG_ERR("TBM", "Truncated bookmark file at entry %u", i);
      break;
    }
    bm.snippet[TXT_BOOKMARK_SNIPPET_MAX - 1] = '\0';
    out.push_back(bm);
  }

  return out;
}

bool TxtBookmarkStore::save(const std::string& cachePath, const std::vector<TxtBookmark>& bookmarks) {
  HalFile f;
  if (!Storage.openFileForWrite("TBM", bookmarksPath(cachePath), f)) {
    LOG_ERR("TBM", "Failed to open bookmark file for write");
    return false;
  }

  serialization::writePod(f, TXT_BM_MAGIC);
  serialization::writePod(f, TXT_BM_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(bookmarks.size()));

  for (const auto& bm : bookmarks) {
    serialization::writePod(f, bm.page);
    f.write(bm.snippet, TXT_BOOKMARK_SNIPPET_MAX);
  }

  return true;
}

TxtBookmarkStore::ToggleResult TxtBookmarkStore::toggle(const std::string& cachePath, uint32_t page,
                                                        const char* snippet) {
  std::vector<TxtBookmark> bookmarks = load(cachePath);

  const auto it = std::find_if(bookmarks.begin(), bookmarks.end(),
                               [page](const TxtBookmark& bm) { return bm.page == page; });

  bool added;
  if (it != bookmarks.end()) {
    bookmarks.erase(it);
    added = false;
  } else {
    TxtBookmark bm;
    bm.page = page;
    snprintf(bm.snippet, sizeof(bm.snippet), "%s", snippet ? snippet : "");
    // Keep ascending order by page so the viewer reads top-to-bottom.
    const auto insertAt = std::upper_bound(bookmarks.begin(), bookmarks.end(), bm,
                                           [](const TxtBookmark& a, const TxtBookmark& b) { return a.page < b.page; });
    bookmarks.insert(insertAt, bm);
    added = true;
  }

  if (!save(cachePath, bookmarks)) {
    return ToggleResult::Error;
  }
  return added ? ToggleResult::Added : ToggleResult::Removed;
}

bool TxtBookmarkStore::hasBookmarkForPage(const std::string& cachePath, uint32_t page) {
  const std::vector<TxtBookmark> bookmarks = load(cachePath);
  return std::any_of(bookmarks.begin(), bookmarks.end(), [page](const TxtBookmark& bm) { return bm.page == page; });
}
