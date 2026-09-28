#include "DictNotes.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "Dictionary.h"

namespace {

// One file per dictionary, named by the hash that identifies it everywhere else (flashcard
// associations, the sync wire). Lowercase hex, fixed width, so a listing sorts predictably.
std::string filePath(const uint32_t dictHash) {
  char name[16];
  snprintf(name, sizeof(name), "/%08lx.txt", static_cast<unsigned long>(dictHash));
  return std::string(DictNotes::NOTES_DIR) + name;
}
std::string tmpFilePath(const uint32_t dictHash) {
  char name[16];
  snprintf(name, sizeof(name), "/%08lx.tmp", static_cast<unsigned long>(dictHash));
  return std::string(DictNotes::NOTES_DIR) + name;
}

bool isSpaceByte(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Parsed view of a "word|dictHash|version|text" line. All offsets point into the caller's
// buffer; nothing is copied. A line with fewer than three '|' is malformed and parses as
// word-only with empty text, which loadWindow shows and removeAt can delete.
struct Parsed {
  const char* word = nullptr;
  int wordLen = 0;
  uint32_t dictHash = 0;
  const char* text = nullptr;
  int textLen = 0;
};

uint32_t parseU32(const char* p, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return v;
    v = v * 10 + static_cast<uint32_t>(p[i] - '0');
  }
  return v;
}

Parsed parseLine(const char* line, int len) {
  Parsed r;
  r.word = line;
  r.wordLen = len;
  r.text = line + len;
  int bars[3];
  int found = 0;
  for (int i = 0; i < len && found < 3; i++) {
    if (line[i] == '|') bars[found++] = i;
  }
  if (found < 3) return r;
  r.wordLen = bars[0];
  r.dictHash = parseU32(line + bars[0] + 1, bars[1] - bars[0] - 1);
  // Field 2 (version) is read past: it is written for sync's benefit and nothing reads it yet.
  r.text = line + bars[2] + 1;
  r.textLen = len - bars[2] - 1;
  return r;
}

// Stream a file line by line. Block reads, not byte reads: every HalFile call takes the
// storage mutex, so a byte loop would pay one guarded read per character (the measurement
// behind LookupHistory::forEachLine using the same shape).
bool forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len), void* ctx) {
  HalFile file;
  if (!Storage.openFileForRead("DNOTE", path, file)) return false;

  char lineBuf[512];  // a line carries note text, so wider than LookupHistory's 256
  int lineLen = 0;
  char chunk[64];
  int have = 0;
  int pos = 0;

  for (;;) {
    if (pos == have) {
      have = file.read(chunk, sizeof(chunk));
      if (have <= 0) break;  // EOF or error
      pos = 0;
    }
    const char b = chunk[pos++];
    if (b == '\n' || b == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        if (!fn(ctx, lineBuf, lineLen)) return true;
        lineLen = 0;
      }
      continue;
    }
    if (lineLen < static_cast<int>(sizeof(lineBuf)) - 1) lineBuf[lineLen++] = b;
  }

  if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    fn(ctx, lineBuf, lineLen);
  }
  return true;
}

bool writeLine(HalFile& out, const char* line, int len) {
  if (out.write(line, static_cast<size_t>(len)) != static_cast<size_t>(len)) return false;
  const char nl = '\n';
  return out.write(&nl, 1) == 1;
}

// Trim to at most maxBytes on a UTF-8 boundary: a split sequence reaches the renderer as
// U+FFFD, which these fonts draw as a bare '?'.
size_t utf8Trim(const char* s, size_t len, size_t maxBytes) {
  if (len <= maxBytes) return len;
  size_t n = maxBytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
  return n;
}

// The note text as it goes on disk: newlines and the field separator's enemies removed, capped.
// Tabs and newlines become spaces so one note is always one line.
std::string sanitize(const std::string& text) {
  std::string out;
  out.reserve(text.size() < DictNotes::TEXT_MAX ? text.size() : DictNotes::TEXT_MAX);
  bool lastWasSpace = false;
  for (const char c : text) {
    const char ch = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    if (ch == ' ') {
      if (lastWasSpace || out.empty()) continue;
      lastWasSpace = true;
    } else {
      lastWasSpace = false;
    }
    out.push_back(ch);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  out.resize(utf8Trim(out.data(), out.size(), DictNotes::TEXT_MAX));
  return out;
}

struct CountCtx {
  int lines = 0;
};
bool countLine(void* ctx, const char*, int) {
  static_cast<CountCtx*>(ctx)->lines++;
  return true;
}

struct DupCtx {
  const std::string* word;
  const std::string* text;
  bool found = false;
};
bool dupLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<DupCtx*>(ctx);
  const Parsed p = parseLine(line, len);
  if (static_cast<size_t>(p.wordLen) != c->word->size() || memcmp(p.word, c->word->c_str(), p.wordLen) != 0)
    return true;
  if (static_cast<size_t>(p.textLen) != c->text->size() || memcmp(p.text, c->text->c_str(), p.textLen) != 0)
    return true;
  c->found = true;
  return false;  // stop
}

}  // namespace

bool DictNotes::add(const std::string& word, const uint32_t dictHash, const std::string& text) {
  if (dictHash == 0 || word.empty()) return false;  // no dictionary resolved: nothing to file it under
  const std::string clean = sanitize(text);
  if (clean.empty()) return false;

  const std::string path = filePath(dictHash);

  DupCtx dc{&word, &clean};
  forEachLine(path, dupLine, &dc);
  if (dc.found) return true;  // already kept; nothing to do

  CountCtx cc;
  forEachLine(path, countLine, &cc);
  const int evictSkip = (cc.lines + 1 > MAX_NOTES) ? (cc.lines + 1 - MAX_NOTES) : 0;

  char head[64];
  const int headLen = snprintf(head, sizeof(head), "|%lu|0|", static_cast<unsigned long>(dictHash));

  // The directory is created here rather than at boot: a device that never keeps a note never
  // grows one. mkdir on an existing path is a no-op (Epub::setupCacheDir takes the same shape).
  Storage.mkdir(NOTES_DIR);

  // Fast path: nothing to evict, so a single append. It never touches existing lines, so a
  // power loss can only leave a torn final line, which parses as one deletable note.
  if (evictSkip == 0) {
    HalFile out;
    if (!Storage.openFileForAppend("DNOTE", path.c_str(), out)) {
      LOG_ERR("DNOTE", "Failed to open for append: %s", path.c_str());
      return false;
    }
    const bool ok = out.write(word.c_str(), word.size()) == word.size() &&
                    out.write(head, static_cast<size_t>(headLen)) == static_cast<size_t>(headLen) &&
                    writeLine(out, clean.c_str(), static_cast<int>(clean.size()));
    out.close();
    if (!ok) LOG_ERR("DNOTE", "Note append failed: %s", path.c_str());
    return ok;
  }

  // Over cap: copy the survivors to a temp file, append the new note, and swap only after a
  // clean write, so a failure mid-write cannot lose the existing notes.
  const std::string tmpPath = tmpFilePath(dictHash);
  {
    HalFile out;
    if (!Storage.openFileForWrite("DNOTE", tmpPath, out)) {
      LOG_ERR("DNOTE", "Failed to open temp for write: %s", tmpPath.c_str());
      return false;
    }
    struct CopyCtx {
      HalFile* out;
      int seen;
      int evictSkip;
      bool ok;
    } wc{&out, 0, evictSkip, true};
    forEachLine(
        path,
        [](void* ctx, const char* line, int len) {
          auto* c = static_cast<CopyCtx*>(ctx);
          if (c->seen++ < c->evictSkip) return true;  // evict oldest over cap
          c->ok = writeLine(*c->out, line, len);
          return c->ok;
        },
        &wc);
    wc.ok = wc.ok && out.write(word.c_str(), word.size()) == word.size() &&
            out.write(head, static_cast<size_t>(headLen)) == static_cast<size_t>(headLen) &&
            writeLine(out, clean.c_str(), static_cast<int>(clean.size()));
    out.close();  // explicit: must be closed before the remove/rename below
    if (!wc.ok) {
      LOG_ERR("DNOTE", "Note write failed: %s", tmpPath.c_str());
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }
  Storage.remove(path.c_str());
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("DNOTE", "Note rename failed: %s", path.c_str());
    return false;
  }
  return true;
}

int DictNotes::count(const uint32_t dictHash) {
  if (dictHash == 0) return 0;
  CountCtx cc;
  forEachLine(filePath(dictHash), countLine, &cc);
  return cc.lines;
}

namespace {

struct WindowCtx {
  int firstWanted;  // file index (oldest = 0) of the first line to keep
  int lastWanted;   // inclusive
  int seen = 0;
  int filled = 0;
  DictNotes::Note* out = nullptr;
};

bool windowLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<WindowCtx*>(ctx);
  const int idx = c->seen++;
  if (idx < c->firstWanted) return true;
  if (idx > c->lastWanted) return false;  // past the window: stop reading
  const Parsed p = parseLine(line, len);
  DictNotes::Note& n = c->out[c->filled++];
  n.word.assign(p.word, static_cast<size_t>(p.wordLen));
  n.text.assign(p.text, static_cast<size_t>(p.textLen));
  n.dictHash = p.dictHash;
  return true;
}

}  // namespace

int DictNotes::loadWindow(const uint32_t dictHash, const int startNewest, const int n, Note* out) {
  if (!out || n <= 0 || startNewest < 0 || dictHash == 0) return 0;
  const int total = count(dictHash);
  if (startNewest >= total) return 0;

  // Newest-first window [startNewest, startNewest + n) maps onto the file (oldest first) as a
  // contiguous run ending at total - 1 - startNewest.
  const int lastWanted = total - 1 - startNewest;
  const int firstWanted = lastWanted - n + 1 > 0 ? lastWanted - n + 1 : 0;

  WindowCtx wc{firstWanted, lastWanted};
  wc.out = out;
  forEachLine(filePath(dictHash), windowLine, &wc);

  // Read oldest-first; the caller wants newest-first.
  for (int i = 0, j = wc.filled - 1; i < j; i++, j--) std::swap(out[i], out[j]);
  return wc.filled;
}

bool DictNotes::removeAt(const uint32_t dictHash, const int indexNewestFirst) {
  if (dictHash == 0 || indexNewestFirst < 0) return false;
  const int total = count(dictHash);
  if (indexNewestFirst >= total) return false;
  const int fileIndex = total - 1 - indexNewestFirst;

  const std::string path = filePath(dictHash);
  const std::string tmpPath = tmpFilePath(dictHash);
  {
    HalFile out;
    if (!Storage.openFileForWrite("DNOTE", tmpPath, out)) {
      LOG_ERR("DNOTE", "Failed to open temp for write: %s", tmpPath.c_str());
      return false;
    }
    struct DropCtx {
      HalFile* out;
      int drop;
      int seen;
      bool ok;
    } dc{&out, fileIndex, 0, true};
    forEachLine(
        path,
        [](void* ctx, const char* line, int len) {
          auto* c = static_cast<DropCtx*>(ctx);
          if (c->seen++ == c->drop) return true;  // the one being removed
          c->ok = writeLine(*c->out, line, len);
          return c->ok;
        },
        &dc);
    out.close();  // explicit: must be closed before the remove/rename below
    if (!dc.ok) {
      LOG_ERR("DNOTE", "Note rewrite failed: %s", tmpPath.c_str());
      Storage.remove(tmpPath.c_str());
      return false;
    }
  }
  Storage.remove(path.c_str());
  if (total == 1) {
    // That was the last note: leave no empty file behind, so the dictionary drops out of the
    // picker rather than listing with nothing in it.
    Storage.remove(tmpPath.c_str());
    return true;
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("DNOTE", "Note rename failed: %s", path.c_str());
    return false;
  }
  return true;
}

namespace {

struct MarkCtx {
  const std::string* word;
  DictNotes::Mark* out;
  int cap;
  int filled = 0;
};

bool markLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<MarkCtx*>(ctx);
  const Parsed p = parseLine(line, len);
  if (p.textLen <= 0) return true;
  if (static_cast<size_t>(p.wordLen) != c->word->size() || memcmp(p.word, c->word->c_str(), p.wordLen) != 0)
    return true;

  // Newest wins when there are more notes than slots: overwrite the oldest kept, shifting the
  // rest down, so the array always holds the newest `cap` in file order.
  if (c->filled == c->cap) {
    for (int i = 1; i < c->cap; i++) c->out[i - 1] = c->out[i];
    c->filled--;
  }
  DictNotes::Mark& m = c->out[c->filled++];
  const size_t n = utf8Trim(p.text, static_cast<size_t>(p.textLen), DictNotes::MARK_TEXT_MAX);
  memcpy(m.text, p.text, n);
  m.text[n] = '\0';
  m.len = static_cast<uint16_t>(n);
  return true;
}

}  // namespace

int DictNotes::loadMarksForWord(const std::string& word, const uint32_t dictHash, Mark* out, const int cap) {
  if (!out || cap <= 0 || dictHash == 0 || word.empty()) return 0;
  MarkCtx mc{&word, out, cap};
  forEachLine(filePath(dictHash), markLine, &mc);
  return mc.filled;
}

namespace {

// "<8 hex>.txt" -> the hash it names. Returns false for anything else in the directory: a temp
// file from an interrupted rewrite, or something the user dropped there.
bool hashFromFileName(const char* name, uint32_t& out) {
  if (!name) return false;
  size_t i = 0;
  uint32_t v = 0;
  for (; i < 8; i++) {
    const char c = name[i];
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a') + 10;
    } else {
      return false;
    }
    v = (v << 4) | digit;
  }
  if (strcmp(name + i, ".txt") != 0) return false;
  out = v;
  return true;
}

}  // namespace

int DictNotes::listDictionaries(DictSummary* out, const int cap) {
  if (!out || cap <= 0) return 0;
  int filled = 0;
  // One argument: the device signature takes an optional maxFiles the host stub does not have.
  const auto names = Storage.listFiles(NOTES_DIR);
  for (const auto& name : names) {
    if (filled >= cap) {
      LOG_ERR("DNOTE", "more than %d dictionaries with notes; listing the first %d", cap, cap);
      break;
    }
    uint32_t hash = 0;
    if (!hashFromFileName(name.c_str(), hash)) continue;
    const int n = count(hash);
    if (n <= 0) continue;  // an empty file is not a dictionary with notes
    out[filled].dictHash = hash;
    out[filled].count = static_cast<uint16_t>(n > UINT16_MAX ? UINT16_MAX : n);
    filled++;
  }
  return filled;
}

// ---------------------------------------------------------------------------
// Token-sequence matching
// ---------------------------------------------------------------------------

namespace {

// Where one token sits: which segment, and its byte range within it.
struct Tok {
  int seg = 0;
  uint16_t start = 0;
  uint16_t end = 0;  // exclusive
};

// Cursor over the whole page's segments, walked once.
struct Cursor {
  int seg = 0;
  uint16_t pos = 0;
};

bool nextToken(const DictNotes::Segment* segs, const int segCount, Cursor& c, Tok& out) {
  while (c.seg < segCount) {
    const DictNotes::Segment& s = segs[c.seg];
    if (!s.text) {
      c.seg++;
      c.pos = 0;
      continue;
    }
    while (c.pos < s.len && isSpaceByte(s.text[c.pos])) c.pos++;
    if (c.pos >= s.len) {
      c.seg++;
      c.pos = 0;
      continue;
    }
    out.seg = c.seg;
    out.start = c.pos;
    while (c.pos < s.len && !isSpaceByte(s.text[c.pos])) c.pos++;
    out.end = c.pos;
    return true;
  }
  return false;
}

// A page token the word extractor would have dropped, so a saved note can never contain it.
// Dictionary::cleanWord is the same rule extractWordsFromLayout applies, which is why it is
// used here rather than a second punctuation table that could drift from it. Only called on
// the mid-phrase mismatch path, so the allocation it makes is rare.
bool isPunctuationOnly(const char* text, const size_t len) {
  return Dictionary::cleanWord(std::string(text, len)).empty();
}

constexpr int kMaxNeedleTokens = 32;
constexpr int kMaxCoverSegs = 8;

}  // namespace

int DictNotes::findSpans(const Segment* segs, const int segCount, const char* needle, Span* out, const int cap) {
  if (!segs || segCount <= 0 || !needle || !out || cap <= 0) return 0;

  // Tokenize the needle once. A note longer than kMaxNeedleTokens tokens matches on its first
  // tokens only, which marks a shorter span rather than none.
  const Segment needleSeg{needle, static_cast<uint16_t>(strlen(needle))};
  Tok needleToks[kMaxNeedleTokens];
  int needleCount = 0;
  {
    Cursor nc;
    Tok t;
    while (needleCount < kMaxNeedleTokens && nextToken(&needleSeg, 1, nc, t)) needleToks[needleCount++] = t;
  }
  if (needleCount == 0) return 0;

  const auto needleTok = [&](const int i, const char*& p, size_t& len) {
    p = needle + needleToks[i].start;
    len = static_cast<size_t>(needleToks[i].end - needleToks[i].start);
  };

  int written = 0;
  int matched = 0;  // needle tokens matched so far
  Cursor cursor;
  Cursor afterFirst;  // where to resume when a partial match fails
  Span cover[kMaxCoverSegs];
  int coverCount = 0;

  Tok tok;
  while (nextToken(segs, segCount, cursor, tok)) {
    const char* np = nullptr;
    size_t nlen = 0;
    needleTok(matched, np, nlen);
    const size_t tokLen = static_cast<size_t>(tok.end - tok.start);
    const char* tokText = segs[tok.seg].text + tok.start;

    if (tokLen == nlen && memcmp(tokText, np, nlen) == 0) {
      if (matched == 0) {
        afterFirst = cursor;
        coverCount = 0;
      }
      // Extend the cover for this segment, or open a new one.
      if (coverCount > 0 && cover[coverCount - 1].segIndex == static_cast<uint16_t>(tok.seg)) {
        Span& s = cover[coverCount - 1];
        s.byteLen = static_cast<uint16_t>(tok.end - s.byteStart);
      } else if (coverCount < kMaxCoverSegs) {
        cover[coverCount++] = Span{static_cast<uint16_t>(tok.seg), tok.start, static_cast<uint16_t>(tokLen)};
      }
      if (++matched == needleCount) {
        for (int i = 0; i < coverCount && written < cap; i++) out[written++] = cover[i];
        matched = 0;
        coverCount = 0;
        if (written >= cap) break;
      }
      continue;
    }

    if (matched > 0) {
      // A token the selection could not have contained sits inside the run: step over it.
      if (isPunctuationOnly(tokText, tokLen)) continue;
      // Otherwise the run is broken. Resume from the token after the one that opened it, so an
      // occurrence starting there is still found.
      matched = 0;
      coverCount = 0;
      cursor = afterFirst;
    }
  }
  return written;
}
