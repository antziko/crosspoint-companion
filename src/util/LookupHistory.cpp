#include "LookupHistory.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "DictStopwords.h"

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string LookupHistory::filePath(const std::string& cachePath) { return cachePath + "/" + FILE_NAME; }

std::string LookupHistory::tmpFilePath(const std::string& cachePath) { return cachePath + "/dictionary_history.tmp"; }

std::string LookupHistory::tombFilePath(const std::string& cachePath) { return cachePath + "/dictionary_history.tomb"; }

std::string LookupHistory::verFilePath(const std::string& cachePath) { return cachePath + "/dictionary_history.ver"; }

namespace {

bool isAllDigits(const char* p, int n) {
  if (n <= 0) return false;
  for (int i = 0; i < n; i++)
    if (p[i] < '0' || p[i] > '9') return false;
  return true;
}

uint32_t parseU32(const char* p, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) v = v * 10 + static_cast<uint32_t>(p[i] - '0');
  return v;
}

// Parsed fields of a "word|STATUS|VER" / "word|STATUS" / "word" line.
struct Parsed {
  int wordLen;
  char status;   // raw status char ('X' default)
  uint32_t ver;  // 0 for legacy lines
};

// Right-to-left parse. STATUS letters (D/T/Y/S/X) are never digits, so an
// all-digit field after the last '|' is unambiguously VER. This also parses the
// 2-field tombstone payload "word|VER" (no status -> 'X', which tombstones ignore).
Parsed parseEntry(const char* line, int len) {
  Parsed r{len, 'X', 0};
  int p2 = -1;
  for (int i = len - 1; i >= 0; i--)
    if (line[i] == '|') {
      p2 = i;
      break;
    }
  if (p2 < 0) return r;  // bare "word"
  const int tail2 = len - (p2 + 1);
  if (isAllDigits(line + p2 + 1, tail2)) {
    // "...|VER": trailing digits are the version.
    r.ver = parseU32(line + p2 + 1, tail2);
    int p1 = -1;
    for (int i = p2 - 1; i >= 0; i--)
      if (line[i] == '|') {
        p1 = i;
        break;
      }
    if (p1 >= 0 && p2 - p1 == 2) {  // "word|S|VER"
      r.status = line[p1 + 1];
      r.wordLen = p1;
    } else {  // "word|VER" (tombstone payload) or malformed status
      r.wordLen = p2;
    }
    return r;
  }
  if (tail2 == 1) {  // legacy "word|S"
    r.status = line[p2 + 1];
    r.wordLen = p2;
  }
  return r;  // otherwise treat the whole line as the word
}

LookupHistory::Status statusFromChar(char c) {
  switch (c) {
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

int wordLenOf(const char* line, int len) { return parseEntry(line, len).wordLen; }
LookupHistory::Status statusOf(const char* line, int len) { return statusFromChar(parseEntry(line, len).status); }
uint32_t verOf(const char* line, int len) { return parseEntry(line, len).ver; }

bool lineWordEquals(const char* line, int len, const std::string& word) {
  const int wordLen = wordLenOf(line, len);
  return static_cast<size_t>(wordLen) == word.size() && memcmp(line, word.c_str(), wordLen) == 0;
}

bool writeLine(HalFile& out, const char* line, int len) {
  if (out.write(line, static_cast<size_t>(len)) != static_cast<size_t>(len)) return false;
  const char nl = '\n';
  return out.write(&nl, 1) == 1;
}

// Write a full "word|STATUS|VER\n" history entry.
bool writeEntry(HalFile& out, const char* word, size_t wordLen, LookupHistory::Status status, uint32_t ver) {
  char tail[16];
  const int n = snprintf(tail, sizeof(tail), "|%c|%lu", static_cast<char>(status), static_cast<unsigned long>(ver));
  if (out.write(word, wordLen) != wordLen) return false;
  return writeLine(out, tail, n);
}

struct CountCtx {
  const std::string* dropWord;  // nullptr = count every line
  int count;
  bool dupSeen = false;  // set when a line matching dropWord is encountered
};

bool countLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<CountCtx*>(ctx);
  if (!c->dropWord || !lineWordEquals(line, len, *c->dropWord))
    c->count++;
  else
    c->dupSeen = true;
  return true;
}

}  // namespace

bool LookupHistory::forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len), void* ctx) {
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

namespace {

// Single-number Lamport counter file IO (dictionary_history.ver).
uint32_t readCounterFile(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("LH", path, f)) return 0;
  char buf[16];
  int n = 0;
  while (f.available() && n < 15) {
    const int b = f.read();
    if (b < '0' || b > '9') break;
    buf[n++] = static_cast<char>(b);
  }
  return n ? parseU32(buf, n) : 0;
}

bool writeCounterFile(const std::string& path, uint32_t v) {
  HalFile f;
  if (!Storage.openFileForWrite("LH", path, f)) return false;
  char buf[16];
  const int n = snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v));
  const bool ok = f.write(buf, static_cast<size_t>(n)) == static_cast<size_t>(n);
  f.close();
  return ok;
}

struct MaxVerCtx {
  uint32_t mx;
};
bool maxVerLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<MaxVerCtx*>(ctx);
  const uint32_t v = verOf(line, len);
  if (v > c->mx) c->mx = v;
  return true;
}

}  // namespace

uint32_t LookupHistory::loadCounter(const std::string& cachePath) {
  const std::string verPath = verFilePath(cachePath);
  uint32_t cur = readCounterFile(verPath);
  if (cur == 0) {
    // .ver missing/zero: reconstruct as the max version across history + tombstones
    // (covers upgrade from unversioned files and a lost counter), then persist.
    MaxVerCtx mc{0};
    forEachLine(filePath(cachePath), maxVerLine, &mc);
    forEachLine(tombFilePath(cachePath), maxVerLine, &mc);
    cur = mc.mx;
    if (cur > 0) writeCounterFile(verPath, cur);
  }
  return cur;
}

uint32_t LookupHistory::nextVersion(const std::string& cachePath) {
  const uint32_t v = loadCounter(cachePath) + 1;
  writeCounterFile(verFilePath(cachePath), v);
  return v;
}

void LookupHistory::observeVersion(const std::string& cachePath, uint32_t v) {
  if (v > loadCounter(cachePath)) writeCounterFile(verFilePath(cachePath), v);
}

int LookupHistory::addWord(const std::string& cachePath, const std::string& word, Status status) {
  if (word.empty()) return 0;
  return addWordVer(cachePath, word, status, nextVersion(cachePath));
}

int LookupHistory::addWordVer(const std::string& cachePath, const std::string& word, Status status, uint32_t version) {
  if (word.empty()) return 0;

  const std::string path = filePath(cachePath);
  const std::string tmpPath = tmpFilePath(cachePath);

  // Pass 1: count the entries that survive dedup (everything except `word`) and
  // note whether `word` is already present.
  CountCtx cc{&word, 0};
  forEachLine(path, countLine, &cc);  // missing file -> 0 survivors

  const bool unlimited = SETTINGS.isLookupHistoryUnlimited();
  const int cap = SETTINGS.getLookupHistoryCapValue();
  const int evictSkip = (!unlimited && cc.count + 1 > cap) ? (cc.count + 1 - cap) : 0;

  // A re-looked-up word un-deletes itself: drop any tombstone so a stale remote
  // delete can't resurrect over this newer add on merge.
  clearTombstone(cachePath, word);

  // Fast path: brand-new word with no eviction needed -> a single append. Avoids
  // the full temp-file rewrite the common case otherwise pays on every lookup.
  // The append never touches existing lines, so prior history is strictly safer
  // than under the rewrite path; a power-loss mid-append can only leave a torn
  // final line, which forEachLine parses as one (deletable) junk entry and the
  // next dup/evict rewrite drops.
  if (!cc.dupSeen && evictSkip == 0) {
    HalFile out;
    if (!Storage.openFileForAppend("LH", path.c_str(), out)) {
      LOG_ERR("LH", "Failed to open for append: %s", path.c_str());
      return -1;
    }
    const bool ok = writeEntry(out, word.c_str(), word.size(), status, version);
    out.close();
    if (!ok) {
      LOG_ERR("LH", "History append failed: %s", path.c_str());
      return -1;
    }
    return cc.count + 1;
  }

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

    wc.ok = wc.ok && writeEntry(out, word.c_str(), word.size(), status, version);
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
  if (DictStopwords::isStopword(word)) return;  // skip common closed-class words (see DictStopwords.h)
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

int LookupHistory::count(const std::string& cachePath) {
  CountCtx cc{nullptr, 0};
  forEachLine(filePath(cachePath), countLine, &cc);  // missing file -> 0
  return cc.count;
}

int LookupHistory::loadWindow(const std::string& cachePath, int startNewest, int n, Entry* out) {
  if (startNewest < 0 || n <= 0 || !out) return 0;
  const std::string path = filePath(cachePath);

  CountCtx cc{nullptr, 0};
  if (!forEachLine(path, countLine, &cc) || cc.count == 0) return 0;
  const int total = cc.count;
  if (startNewest >= total) return 0;

  // Newest-first window [startNewest, startNewest+want) maps to the contiguous
  // oldest-first file range [lo, hi]. One forward pass captures those lines and
  // places them newest-first into out[] (out[0] = most recent of the window).
  const int want = std::min(n, total - startNewest);
  const int hi = total - 1 - startNewest;  // file index of out[0]
  const int lo = total - 1 - (startNewest + want - 1);

  struct WinCtx {
    int fileIdx;
    int lo;
    int hi;
    Entry* out;
  } wc{0, lo, hi, out};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<WinCtx*>(ctx);
        const int idx = c->fileIdx++;
        if (idx < c->lo || idx > c->hi) return true;
        const int wordLen = wordLenOf(line, len);
        Entry& e = c->out[c->hi - idx];  // newest-first offset
        e.word.assign(line, static_cast<size_t>(wordLen));
        e.status = statusOf(line, len);
        return c->fileIdx <= c->hi;  // stop once past the window
      },
      &wc);
  return want;
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

bool LookupHistory::getEntryNewestFirst(const std::string& cachePath, int index, Entry& out) {
  return loadWindow(cachePath, index, 1, &out) == 1;
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
    std::string removedWord;  // captured at skipIdx so we can tombstone it
  } wc{&out, 0, index, true, {}};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<CopyCtx*>(ctx);
        if (c->seen++ != c->skipIdx) {
          c->ok = writeLine(*c->out, line, len);
        } else {
          c->removedWord.assign(line, static_cast<size_t>(wordLenOf(line, len)));
        }
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
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) return false;

  // Record a tombstone (with a fresh version) so the delete propagates on sync.
  if (!wc.removedWord.empty()) setTombstone(cachePath, wc.removedWord, nextVersion(cachePath));
  return true;
}

// ---------------------------------------------------------------------------
// Tombstones + cross-device merge
// ---------------------------------------------------------------------------

int LookupHistory::historyVersionOf(const std::string& cachePath, const std::string& word) {
  struct VerCtx {
    const std::string* word;
    int ver;  // -1 = not found
  } hc{&word, -1};
  forEachLine(
      filePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<VerCtx*>(ctx);
        if (lineWordEquals(line, len, *c->word)) {
          const int v = static_cast<int>(verOf(line, len));
          if (v > c->ver) c->ver = v;
        }
        return true;
      },
      &hc);
  return hc.ver;
}

uint32_t LookupHistory::tombstoneVersionOf(const std::string& cachePath, const std::string& word) {
  struct VerCtx {
    const std::string* word;
    uint32_t ver;
  } tc{&word, 0};
  forEachLine(
      tombFilePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<VerCtx*>(ctx);
        if (lineWordEquals(line, len, *c->word)) {
          const uint32_t v = verOf(line, len);
          if (v > c->ver) c->ver = v;
        }
        return true;
      },
      &tc);
  return tc.ver;
}

void LookupHistory::setTombstone(const std::string& cachePath, const std::string& word, uint32_t version) {
  const std::string tomb = tombFilePath(cachePath);
  const std::string tmp = cachePath + "/dictionary_history.tomb.tmp";
  HalFile out;
  if (!Storage.openFileForWrite("LH", tmp, out)) {
    LOG_ERR("LH", "Failed to open tomb temp: %s", tmp.c_str());
    return;
  }
  struct C {
    const std::string* word;
    HalFile* out;
    bool ok;
  } wc{&word, &out, true};
  forEachLine(
      tomb,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<C*>(ctx);
        if (!lineWordEquals(line, len, *c->word)) c->ok = c->ok && writeLine(*c->out, line, len);
        return c->ok;
      },
      &wc);
  char tail[16];
  const int n = snprintf(tail, sizeof(tail), "|%lu", static_cast<unsigned long>(version));
  wc.ok = wc.ok && out.write(word.c_str(), word.size()) == word.size() && writeLine(out, tail, n);
  out.close();
  if (!wc.ok) {
    LOG_ERR("LH", "Tombstone write failed: %s", tmp.c_str());
    Storage.remove(tmp.c_str());
    return;
  }
  Storage.remove(tomb.c_str());
  Storage.rename(tmp.c_str(), tomb.c_str());
}

void LookupHistory::clearTombstone(const std::string& cachePath, const std::string& word) {
  const std::string tomb = tombFilePath(cachePath);
  // Scan first: avoid a rewrite when there's no tombstone (the common case).
  struct P {
    const std::string* word;
    bool found;
  } pc{&word, false};
  forEachLine(
      tomb,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<P*>(ctx);
        if (lineWordEquals(line, len, *c->word)) {
          c->found = true;
          return false;
        }
        return true;
      },
      &pc);
  if (!pc.found) return;

  const std::string tmp = cachePath + "/dictionary_history.tomb.tmp";
  HalFile out;
  if (!Storage.openFileForWrite("LH", tmp, out)) return;
  struct C {
    const std::string* word;
    HalFile* out;
    bool ok;
  } wc{&word, &out, true};
  forEachLine(
      tomb,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<C*>(ctx);
        if (!lineWordEquals(line, len, *c->word)) c->ok = c->ok && writeLine(*c->out, line, len);
        return c->ok;
      },
      &wc);
  out.close();
  if (!wc.ok) {
    Storage.remove(tmp.c_str());
    return;
  }
  Storage.remove(tomb.c_str());
  Storage.rename(tmp.c_str(), tomb.c_str());
}

void LookupHistory::removeWord(const std::string& cachePath, const std::string& word) {
  const std::string path = filePath(cachePath);
  // Scan first: skip the rewrite when the word isn't present.
  struct P {
    const std::string* word;
    bool found;
  } pc{&word, false};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<P*>(ctx);
        if (lineWordEquals(line, len, *c->word)) {
          c->found = true;
          return false;
        }
        return true;
      },
      &pc);
  if (!pc.found) return;

  const std::string tmpPath = tmpFilePath(cachePath);
  HalFile out;
  if (!Storage.openFileForWrite("LH", tmpPath, out)) return;
  struct C {
    const std::string* word;
    HalFile* out;
    bool ok;
  } wc{&word, &out, true};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<C*>(ctx);
        if (!lineWordEquals(line, len, *c->word)) c->ok = c->ok && writeLine(*c->out, line, len);
        return c->ok;
      },
      &wc);
  out.close();
  if (!wc.ok) {
    Storage.remove(tmpPath.c_str());
    return;
  }
  Storage.remove(path.c_str());
  Storage.rename(tmpPath.c_str(), path.c_str());
}

size_t LookupHistory::serializeBlob(const std::string& cachePath, uint8_t* out, size_t cap, int* outHistCount,
                                    int* outTombCount) {
  if (outHistCount) *outHistCount = 0;
  if (outTombCount) *outTombCount = 0;
  if (!out || cap == 0) return 0;

  // Tombstones first ("T" + "word|VER") so deletions always propagate.
  struct TCtx {
    uint8_t* out;
    size_t cap;
    size_t used;
    int count;
  } tw{out, cap, 0, 0};
  forEachLine(
      tombFilePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<TCtx*>(ctx);
        const size_t need = 1 + static_cast<size_t>(len) + 1;
        if (c->used + need > c->cap) return false;  // budget full
        c->out[c->used++] = 'T';
        memcpy(c->out + c->used, line, static_cast<size_t>(len));
        c->used += static_cast<size_t>(len);
        c->out[c->used++] = '\n';
        c->count++;
        return true;
      },
      &tw);
  if (outTombCount) *outTombCount = tw.count;

  // History newest-first into the remaining budget. Pass A: total blob cost.
  struct SumCtx {
    size_t total;
  } sc{0};
  forEachLine(
      filePath(cachePath),
      [](void* ctx, const char* line, int len) {
        static_cast<SumCtx*>(ctx)->total += static_cast<size_t>(len) + 2;  // 'H' + line + '\n'
        return true;
      },
      &sc);

  // Pass B: skip oldest until the remaining suffix fits the budget, emit the rest.
  struct HCtx {
    uint8_t* out;
    size_t used;
    size_t remaining;
    size_t budget;
    int histCount;
  } hc{out, tw.used, sc.total, cap - tw.used, 0};
  forEachLine(
      filePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<HCtx*>(ctx);
        const size_t cost = static_cast<size_t>(len) + 2;
        if (c->remaining > c->budget) {  // still trimming the oldest
          c->remaining -= cost;
          return true;
        }
        c->out[c->used++] = 'H';
        memcpy(c->out + c->used, line, static_cast<size_t>(len));
        c->used += static_cast<size_t>(len);
        c->out[c->used++] = '\n';
        c->histCount++;
        return true;
      },
      &hc);
  if (outHistCount) *outHistCount = hc.histCount;
  return hc.used;
}

int LookupHistory::mergeBlob(const std::string& cachePath, const uint8_t* blob, size_t len, int* outDeleted) {
  if (outDeleted) *outDeleted = 0;
  if (!blob || len == 0) return 0;
  loadCounter(cachePath);  // materialize the Lamport clock before observeVersion calls

  int added = 0;
  int deleted = 0;

  // Two passes over the in-memory blob: apply tombstones first, then adds, so a
  // re-lookup (higher version) correctly beats a delete for the same word.
  for (int pass = 0; pass < 2; pass++) {
    const char want = (pass == 0) ? 'T' : 'H';
    size_t i = 0;
    while (i < len) {
      size_t j = i;
      while (j < len && blob[j] != '\n') j++;
      const char* lineStart = reinterpret_cast<const char*>(blob + i);
      const int lineLen = static_cast<int>(j - i);
      i = j + 1;
      if (lineLen < 2 || lineStart[0] != want) continue;

      const char* payload = lineStart + 1;
      const int plen = lineLen - 1;
      const Parsed p = parseEntry(payload, plen);
      if (p.wordLen <= 0) continue;
      std::string word;
      word.assign(payload, static_cast<size_t>(p.wordLen));

      observeVersion(cachePath, p.ver);

      const int histVer = historyVersionOf(cachePath, word);  // -1 if absent
      const uint32_t tombVer = tombstoneVersionOf(cachePath, word);

      if (want == 'T') {
        // Apply a remote delete only if it is newer than what we know about the
        // word (its history version, else its tombstone version, else nothing).
        const uint32_t localVer = std::max(histVer >= 0 ? static_cast<uint32_t>(histVer) : 0u, tombVer);
        if (p.ver <= localVer) continue;
        removeWord(cachePath, word);
        setTombstone(cachePath, word, p.ver);
        deleted++;
      } else {
        // Apply a remote add if: the word is entirely new to us (covers legacy v0
        // entries, which must merge even though their version ties absent==0), OR
        // it is newer than our copy, OR it beats a tombstone that deleted it.
        if (histVer >= 0) {
          if (p.ver <= static_cast<uint32_t>(histVer)) continue;  // have it, not newer
        } else if (tombVer > 0 && p.ver <= tombVer) {
          continue;  // a newer/equal delete still wins
        }
        addWordVer(cachePath, word, statusFromChar(p.status), p.ver);
        added++;
      }
    }
  }
  if (outDeleted) *outDeleted = deleted;
  return added;
}
