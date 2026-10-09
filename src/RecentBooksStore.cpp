#include "RecentBooksStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "util/RecentBooksLine.h"

RecentBooksStore RecentBooksStore::instance;

namespace {
constexpr char HISTORY_PATH[] = "/.crosspoint/recent.tsv";
constexpr char HISTORY_TMP[] = "/.crosspoint/recent.tsv.tmp";
constexpr char LEGACY_PATH[] = "/.crosspoint/recent.json";
// Longest line kept; a longer one is skipped as corrupt. A path, title, author
// and cover path come to a few hundred bytes.
constexpr size_t LINE_CAP = 1024;
constexpr size_t LEGACY_CAP = 16 * 1024;

// Lines from a HalFile, read a block at a time: every HalFile call takes the
// storage mutex, so a byte-at-a-time read would cost one lock per character.
class LineReader {
 public:
  LineReader(HalFile& file, char* buf, const size_t cap) : file(file), buf(buf), cap(cap) {}

  // The next line without its '\n', and the file offset it starts at. False at the end.
  bool next(std::string_view& line, uint32_t& offset) {
    for (;;) {
      const auto* newline = static_cast<const char*>(memchr(buf + pos, '\n', len - pos));
      if (newline) {
        const size_t start = pos;
        const size_t length = static_cast<size_t>(newline - (buf + start));
        pos = start + length + 1;
        if (skipping) {
          skipping = false;
          continue;
        }
        line = std::string_view(buf + start, length);
        offset = base + static_cast<uint32_t>(start);
        return true;
      }
      if (pos > 0) {
        memmove(buf, buf + pos, len - pos);
        base += static_cast<uint32_t>(pos);
        len -= pos;
        pos = 0;
      }
      if (len == cap) {
        // No newline within LINE_CAP: drop this line through its end.
        base += static_cast<uint32_t>(len);
        len = 0;
        skipping = true;
      }
      if (eof) {
        if (len == 0 || skipping) return false;
        // A final line with no newline.
        line = std::string_view(buf, len);
        offset = base;
        base += static_cast<uint32_t>(len);
        len = 0;
        return true;
      }
      const int got = file.read(buf + len, cap - len);
      if (got <= 0) {
        eof = true;
      } else {
        len += static_cast<size_t>(got);
      }
    }
  }

 private:
  HalFile& file;
  char* buf;
  size_t cap;
  size_t len = 0;
  size_t pos = 0;
  uint32_t base = 0;  // file offset of buf[0]
  bool eof = false;
  bool skipping = false;
};

bool openHistory(HalFile& file) {
  return Storage.exists(HISTORY_PATH) && Storage.openFileForRead("RBS", HISTORY_PATH, file);
}

// Calls fn(fields, offset) for each well-formed line until it returns false.
template <typename Fn>
bool forEachLine(Fn&& fn) {
  HalFile file;
  if (!openHistory(file)) return true;  // no history yet
  auto buf = makeUniqueNoThrow<char[]>(LINE_CAP);
  if (!buf) {
    LOG_ERR("RBS", "OOM: %u byte line buffer", static_cast<unsigned>(LINE_CAP));
    return false;
  }
  LineReader reader(file, buf.get(), LINE_CAP);
  std::string_view line;
  uint32_t offset = 0;
  while (reader.next(line, offset)) {
    recentline::Fields fields;
    if (!recentline::decode(line, fields)) continue;
    if (!fn(fields, line, offset)) break;
  }
  return true;
}

enum class Edit { Keep, Drop, Replace };

// Streams the history through `edit` into a replacement file, with `first`
// (if any) as the new first line, and stops at MAX_RECENT_BOOKS entries.
// edit(fields, replacement) returns what to do with each line; Replace writes
// `replacement`, a complete encoded line.
template <typename EditFn>
bool rewriteHistory(const std::string& first, EditFn&& edit) {
  HalFile out;
  if (!Storage.openFileForWrite("RBS", HISTORY_TMP, out)) return false;
  bool ok = true;
  int written = 0;
  const auto put = [&](const std::string_view text) {
    if (out.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) != text.size()) ok = false;
  };
  if (!first.empty()) {
    put(first);
    written++;
  }
  std::string replacement;
  ok = forEachLine([&](const recentline::Fields& fields, const std::string_view line, uint32_t) {
         if (written >= RecentBooksStore::MAX_RECENT_BOOKS) return false;
         replacement.clear();
         switch (edit(fields, replacement)) {
           case Edit::Drop:
             return true;
           case Edit::Keep:
             put(line);
             put("\n");
             break;
           case Edit::Replace:
             put(replacement);
             break;
         }
         written++;
         return ok;
       }) &&
       ok;
  out.flush();
  out.close();
  if (!ok) {
    LOG_ERR("RBS", "Failed to write %s", HISTORY_TMP);
    Storage.remove(HISTORY_TMP);
    return false;
  }
  return Storage.replaceFile(HISTORY_TMP, HISTORY_PATH);
}

bool contains(const std::string& path) {
  bool found = false;
  forEachLine([&](const recentline::Fields& fields, std::string_view, uint32_t) {
    found = fields.path == path;
    return !found;
  });
  return found;
}

bool isUnder(const std::string_view entry, const std::string& path) {
  return entry == path || (entry.size() > path.size() && entry.substr(0, path.size()) == path &&
                           (path.back() == '/' || entry[path.size()] == '/'));
}
}  // namespace

bool RecentBooksStore::loadFromFile() {
  if (!Storage.exists(HISTORY_PATH) && Storage.exists(LEGACY_PATH)) migrateJson();
  return reloadHead();
}

bool RecentBooksStore::reloadHead() {
  head.clear();
  head.reserve(HEAD_COUNT);
  const bool ok = forEachLine([&](const recentline::Fields& fields, std::string_view, uint32_t) {
    head.push_back(RecentBook{std::string(fields.path), std::string(fields.title), std::string(fields.author),
                              std::string(fields.cover)});
    return static_cast<int>(head.size()) < HEAD_COUNT;
  });
  LOG_DBG("RBS", "Recent books head loaded (%u entries)", static_cast<unsigned>(head.size()));
  return ok;
}

// The pre-history store: a JSON array of at most ten books. Converted once,
// then removed so the two can never disagree.
bool RecentBooksStore::migrateJson() {
  std::string raw;
  if (!Storage.readFileToString("RBS", LEGACY_PATH, LEGACY_CAP, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) {
    LOG_ERR("RBS", "Unreadable %s; starting a new history", LEGACY_PATH);
    return false;
  }
  raw.clear();
  std::string lines;
  for (JsonObjectConst obj : doc["books"].as<JsonArrayConst>()) {
    const char* path = obj["path"] | "";
    if (!path[0]) continue;
    lines += recentline::encode(path, obj["title"] | "", obj["author"] | "", obj["coverBmpPath"] | "");
  }
  HalFile out;
  if (!Storage.openFileForWrite("RBS", HISTORY_TMP, out)) return false;
  const bool written = out.write(reinterpret_cast<const uint8_t*>(lines.data()), lines.size()) == lines.size();
  out.flush();
  out.close();
  if (!written || !Storage.replaceFile(HISTORY_TMP, HISTORY_PATH)) {
    Storage.remove(HISTORY_TMP);
    return false;
  }
  Storage.remove(LEGACY_PATH);
  LOG_INF("RBS", "Converted %s to %s", LEGACY_PATH, HISTORY_PATH);
  return true;
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  if (!head.empty()) {
    const RecentBook& front = head.front();
    if (front.path == path && front.title == title && front.author == author && front.coverBmpPath == coverBmpPath) {
      return;
    }
  }
  const bool saved = rewriteHistory(
      recentline::encode(path, title, author, coverBmpPath),
      [&](const recentline::Fields& fields, std::string&) { return fields.path == path ? Edit::Drop : Edit::Keep; });
  if (!saved) {
    LOG_ERR("RBS", "Failed to save recent book: %s", path.c_str());
    return;
  }

  // Mirror the file's new head in place rather than re-reading it. The reader calls this after
  // its first render, so fresh string buffers would land among reader allocations and outlive
  // them, splitting the largest free block once the reader exits.
  auto it = std::find_if(head.begin(), head.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != head.end()) {
    std::rotate(head.begin(), it, it + 1);
    RecentBook& book = head.front();
    if (book.title != title) book.title = title;
    if (book.author != author) book.author = author;
    if (book.coverBmpPath != coverBmpPath) book.coverBmpPath = coverBmpPath;
  } else {
    head.insert(head.begin(), RecentBook{path, title, author, coverBmpPath});
    if (static_cast<int>(head.size()) > HEAD_COUNT) head.pop_back();
  }
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  if (!contains(path)) return false;
  if (!rewriteHistory({}, [&](const recentline::Fields& fields, std::string&) {
        return fields.path == path ? Edit::Drop : Edit::Keep;
      })) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  reloadHead();
  return true;
}

bool RecentBooksStore::removeUnder(const std::string& path) {
  if (path.empty()) return false;
  bool any = false;
  forEachLine([&](const recentline::Fields& fields, std::string_view, uint32_t) {
    any = isUnder(fields.path, path);
    return !any;
  });
  if (!any) return false;
  if (!rewriteHistory({}, [&](const recentline::Fields& fields, std::string&) {
        return isUnder(fields.path, path) ? Edit::Drop : Edit::Keep;
      })) {
    LOG_ERR("RBS", "Failed to persist removal of recent books under: %s", path.c_str());
  }
  reloadHead();
  return true;
}

bool RecentBooksStore::swapEntries(const std::string& pathA, const std::string& pathB) {
  if (pathA == pathB) return false;
  std::string lineA;
  std::string lineB;
  forEachLine([&](const recentline::Fields& fields, const std::string_view line, uint32_t) {
    if (fields.path == pathA) lineA.assign(line.data(), line.size()).append("\n");
    if (fields.path == pathB) lineB.assign(line.data(), line.size()).append("\n");
    return lineA.empty() || lineB.empty();
  });
  if (lineA.empty() || lineB.empty()) return false;
  const bool saved = rewriteHistory({}, [&](const recentline::Fields& fields, std::string& replacement) {
    if (fields.path == pathA) {
      replacement = lineB;
      return Edit::Replace;
    }
    if (fields.path == pathB) {
      replacement = lineA;
      return Edit::Replace;
    }
    return Edit::Keep;
  });
  reloadHead();
  return saved;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  if (!contains(oldPath)) return;
  rewriteHistory({}, [&](const recentline::Fields& fields, std::string& replacement) {
    if (fields.path != oldPath) return Edit::Keep;
    std::string cover(fields.cover);
    if (!oldCachePath.empty() && !cover.empty() && cover.rfind(oldCachePath, 0) == 0) {
      cover = newCachePath + cover.substr(oldCachePath.size());
    }
    replacement = recentline::encode(newPath, fields.title, fields.author, cover);
    return Edit::Replace;
  });
  reloadHead();
}

bool RecentBooksStore::isMissing(const RecentBook& book) { return !Storage.exists(book.path.c_str()); }

bool RecentBooksStore::scan(const ScanFn fn, void* ctx) const {
  return forEachLine([&](const recentline::Fields& fields, std::string_view, const uint32_t offset) {
    return fn(ctx, fields.path, offset);
  });
}

bool RecentBooksStore::readAt(const uint32_t offset, RecentBook& out) const {
  HalFile file;
  if (!openHistory(file) || !file.seek(offset)) return false;
  auto buf = makeUniqueNoThrow<char[]>(LINE_CAP);
  if (!buf) {
    LOG_ERR("RBS", "OOM: %u byte line buffer", static_cast<unsigned>(LINE_CAP));
    return false;
  }
  LineReader reader(file, buf.get(), LINE_CAP);
  std::string_view line;
  uint32_t unused = 0;
  recentline::Fields fields;
  if (!reader.next(line, unused) || !recentline::decode(line, fields)) return false;
  out.path.assign(fields.path);
  out.title.assign(fields.title);
  out.author.assign(fields.author);
  out.coverBmpPath.assign(fields.cover);
  return true;
}
