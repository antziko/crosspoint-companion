#include "LookupHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string LookupHistory::filePath(const std::string& cachePath) {
  return cachePath + "/" + FILE_NAME;
}

std::string LookupHistory::tmpFilePath(const std::string& cachePath) {
  return cachePath + "/dictionary_history.tmp";
}

namespace {

// Length of the word prefix of a "word|STATUS" line. A line with no separator
// (or a trailing separator with no status char) is a legacy entry: the whole
// line is the word.
int wordLenOf(const char* line, int len) {
  for (int i = len - 1; i >= 0; i--) {
    if (line[i] == '|') {
      return (i + 1 < len) ? i : len;
    }
  }
  return len;
}

LookupHistory::Status statusOf(const char* line, int len) {
  const int wordLen = wordLenOf(line, len);
  if (wordLen >= len) return LookupHistory::Status::NotFound;  // legacy line, no status
  switch (line[wordLen + 1]) {
    case 'D':
      return LookupHistory::Status::Direct;
    case 'T':
      return LookupHistory::Status::Stem;
    case 'Y':
      return LookupHistory::Status::AltForm;
    case 'S':
      return LookupHistory::Status::Suggestion;
    default:
      return LookupHistory::Status::NotFound;
  }
}

bool lineWordEquals(const char* line, int len, const std::string& word) {
  const int wordLen = wordLenOf(line, len);
  return static_cast<size_t>(wordLen) == word.size() && memcmp(line, word.c_str(), wordLen) == 0;
}

bool writeLine(HalFile& out, const char* line, int len) {
  if (out.write(line, static_cast<size_t>(len)) != static_cast<size_t>(len)) return false;
  const char nl = '\n';
  return out.write(&nl, 1) == 1;
}

struct CountCtx {
  const std::string* dropWord;  // nullptr = count every line
  int count;
};

bool countLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<CountCtx*>(ctx);
  if (!c->dropWord || !lineWordEquals(line, len, *c->dropWord)) c->count++;
  return true;
}

}  // namespace

bool LookupHistory::forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len),
                                void* ctx) {
  HalFile file;
  if (!Storage.openFileForRead("LH", path, file)) return false;

  char lineBuf[256];
  int lineLen = 0;

  while (file.available()) {
    const int b = file.read();
    if (b < 0) break;

    if (b == '\n' || b == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        if (!fn(ctx, lineBuf, lineLen)) return true;
        lineLen = 0;
      }
      continue;
    }

    if (lineLen < static_cast<int>(sizeof(lineBuf)) - 1) {
      lineBuf[lineLen++] = static_cast<char>(b);
    }
  }

  // Handle last line without trailing newline
  if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    fn(ctx, lineBuf, lineLen);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int LookupHistory::addWord(const std::string& cachePath, const std::string& word, Status status) {
  if (word.empty()) return 0;

  const std::string path = filePath(cachePath);
  const std::string tmpPath = tmpFilePath(cachePath);

  // Pass 1: count the entries that survive dedup (everything except `word`).
  CountCtx cc{&word, 0};
  forEachLine(path, countLine, &cc);  // missing file -> 0 survivors

  const int cap = SETTINGS.getLookupHistoryCapValue();
  const int evictSkip = (cc.count + 1 > cap) ? (cc.count + 1 - cap) : 0;

  // Pass 2: copy the survivors (minus the evicted oldest) to a temp file, then
  // append the new entry. The original is replaced only after a fully clean
  // write so a failure mid-write can't lose the existing history.
  {
    HalFile out;
    if (!Storage.openFileForWrite("LH", tmpPath, out)) {
      LOG_ERR("LH", "Failed to open temp for write: %s", tmpPath.c_str());
      return -1;
    }

    struct CopyCtx {
      const std::string* word;
      HalFile* out;
      int seen;
      int evictSkip;
      bool ok;
    } wc{&word, &out, 0, evictSkip, true};
    forEachLine(
        path,
        [](void* ctx, const char* line, int len) {
          auto* c = static_cast<CopyCtx*>(ctx);
          if (lineWordEquals(line, len, *c->word)) return true;  // dedup: drop the old copy
          if (c->seen++ < c->evictSkip) return true;             // evict oldest over cap
          c->ok = writeLine(*c->out, line, len);
          return c->ok;
        },
        &wc);

    const char tail[2] = {'|', static_cast<char>(status)};
    wc.ok = wc.ok && out.write(word.c_str(), word.size()) == word.size();
    wc.ok = wc.ok && writeLine(out, tail, 2);
    out.close();  // explicit: must be closed before the remove/rename below

    if (!wc.ok) {
      LOG_ERR("LH", "History write failed: %s", tmpPath.c_str());
      Storage.remove(tmpPath.c_str());
      return -1;
    }
  }

  Storage.remove(path.c_str());  // may not exist on first lookup; ignore result
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("LH", "History rename failed: %s", path.c_str());
    return -1;
  }
  return cc.count - evictSkip + 1;
}

void LookupHistory::addWordIf(const std::string& cachePath, const std::string& word, Status status, bool enabled) {
  if (!enabled || word.empty() || cachePath.empty()) return;
  addWord(cachePath, word, status);
}

std::vector<LookupHistory::Entry> LookupHistory::load(const std::string& cachePath) {
  const std::string path = filePath(cachePath);
  std::vector<Entry> entries;

  // Count first so the vector is sized exactly once -- push_back regrowth on a
  // fragmented heap is what OOM-aborted here (see class comment).
  CountCtx cc{nullptr, 0};
  if (!forEachLine(path, countLine, &cc) || cc.count == 0) return entries;
  entries.reserve(cc.count);

  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* v = static_cast<std::vector<Entry>*>(ctx);
        const int wordLen = wordLenOf(line, len);
        if (wordLen == 0) return true;
        Entry e;
        e.word.assign(line, static_cast<size_t>(wordLen));
        e.status = statusOf(line, len);
        v->push_back(std::move(e));
        return true;
      },
      &entries);

  std::reverse(entries.begin(), entries.end());
  return entries;
}

std::string LookupHistory::getWordNewestFirst(const std::string& cachePath, int index) {
  if (index < 0) return "";
  const std::string path = filePath(cachePath);

  CountCtx cc{nullptr, 0};
  if (!forEachLine(path, countLine, &cc)) return "";
  const int target = cc.count - 1 - index;  // newest-first -> oldest-first file index
  if (target < 0) return "";

  struct FetchCtx {
    int seen;
    int target;
    int len;
    char buf[256];
  } fc{0, target, 0, {}};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<FetchCtx*>(ctx);
        if (c->seen++ != c->target) return true;
        c->len = wordLenOf(line, len);
        memcpy(c->buf, line, static_cast<size_t>(c->len));
        return false;  // found -- stop streaming
      },
      &fc);
  return std::string(fc.buf, static_cast<size_t>(fc.len));
}

bool LookupHistory::removeAt(const std::string& cachePath, int index) {
  if (index < 0) return false;
  const std::string path = filePath(cachePath);
  const std::string tmpPath = tmpFilePath(cachePath);

  // Pass 1: bounds-check the index.
  CountCtx cc{nullptr, 0};
  if (!forEachLine(path, countLine, &cc)) return false;
  if (index >= cc.count) return false;

  // Pass 2: copy every line except the index-th.
  HalFile out;
  if (!Storage.openFileForWrite("LH", tmpPath, out)) {
    LOG_ERR("LH", "Failed to open temp for write: %s", tmpPath.c_str());
    return false;
  }
  struct CopyCtx {
    HalFile* out;
    int seen;
    int skipIdx;
    bool ok;
  } wc{&out, 0, index, true};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<CopyCtx*>(ctx);
        if (c->seen++ != c->skipIdx) c->ok = writeLine(*c->out, line, len);
        return c->ok;
      },
      &wc);
  out.close();  // explicit: must be closed before the remove/rename below

  if (!wc.ok) {
    LOG_ERR("LH", "History write failed: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(path.c_str());
  return Storage.rename(tmpPath.c_str(), path.c_str());
}
