#include "FlashcardDeck.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::string FlashcardDeck::filePath(const std::string& cachePath) { return cachePath + "/" + FILE_NAME; }

std::string FlashcardDeck::tmpFilePath(const std::string& cachePath) {
  return cachePath + "/dictionary_flashcards.tmp";
}

// ---------------------------------------------------------------------------
// Line parsing / writing
// ---------------------------------------------------------------------------

namespace {

uint32_t parseU32(const char* p, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') break;
    v = v * 10 + static_cast<uint32_t>(p[i] - '0');
  }
  return v;
}

// Parsed fields of a "word|box|dueDay|chapter|excerpt" line. The excerpt is the
// remainder after the fourth '|', so an embedded '|' in the excerpt is harmless.
// Legacy 4-field lines ("word|box|dueDay|excerpt", three '|') parse with an
// empty chapter and the excerpt intact. Malformed lines (fewer than three '|')
// degrade to box 0 / dueDay 0 / no chapter / no excerpt, whole line = word.
struct Parsed {
  int wordLen;
  uint8_t box;
  uint32_t dueDay;
  const char* chapter;
  int chapterLen;
  const char* excerpt;
  int excerptLen;
};

Parsed parseLine(const char* line, int len) {
  Parsed r{len, 0, 0, line + len, 0, line + len, 0};
  int p0 = -1, p1 = -1, p2 = -1, p3 = -1;
  for (int i = 0; i < len; i++) {
    if (line[i] != '|') continue;
    if (p0 < 0)
      p0 = i;
    else if (p1 < 0)
      p1 = i;
    else if (p2 < 0)
      p2 = i;
    else {
      p3 = i;
      break;
    }
  }
  if (p0 < 0 || p1 < 0 || p2 < 0) return r;  // malformed / legacy: whole line is the word
  r.wordLen = p0;
  r.box = static_cast<uint8_t>(parseU32(line + p0 + 1, p1 - p0 - 1));
  r.dueDay = parseU32(line + p1 + 1, p2 - p1 - 1);
  if (p3 < 0) {
    // Legacy 4-field line: no chapter, excerpt is the remainder after dueDay.
    r.excerpt = line + p2 + 1;
    r.excerptLen = len - (p2 + 1);
  } else {
    r.chapter = line + p2 + 1;
    r.chapterLen = p3 - (p2 + 1);
    r.excerpt = line + p3 + 1;
    r.excerptLen = len - (p3 + 1);
  }
  return r;
}

bool lineWordEquals(const char* line, int len, const std::string& word) {
  const Parsed p = parseLine(line, len);
  return static_cast<size_t>(p.wordLen) == word.size() && memcmp(line, word.c_str(), p.wordLen) == 0;
}

bool writeRaw(HalFile& out, const char* p, int n) {
  return out.write(p, static_cast<size_t>(n)) == static_cast<size_t>(n);
}

// Write a full "word|box|dueDay|chapter|excerpt\n" card line.
bool writeCard(HalFile& out, const char* word, size_t wordLen, uint8_t box, uint32_t dueDay, const char* chapter,
               int chapterLen, const char* excerpt, int excerptLen) {
  char mid[24];
  const int m = snprintf(mid, sizeof(mid), "|%u|%lu|", static_cast<unsigned>(box), static_cast<unsigned long>(dueDay));
  bool ok = writeRaw(out, word, static_cast<int>(wordLen)) && writeRaw(out, mid, m);
  if (ok && chapterLen > 0) ok = writeRaw(out, chapter, chapterLen);
  const char bar = '|';
  if (ok) ok = out.write(&bar, 1) == 1;
  if (ok && excerptLen > 0) ok = writeRaw(out, excerpt, excerptLen);
  const char nl = '\n';
  return ok && out.write(&nl, 1) == 1;
}

// Sanitize a caller-supplied field into `dst`: collapse control chars
// (newlines/tabs) to single spaces and cap to `cap`. When `stripPipe` is set
// (chapter, a delimited field), '|' is also collapsed to a space so it cannot
// break the line parse. Returns length.
int sanitizeField(const std::string& src, char* dst, int cap, bool stripPipe) {
  int n = 0;
  for (char c : src) {
    if (n >= cap) break;
    const unsigned char uc = static_cast<unsigned char>(c);
    dst[n++] = (uc < 0x20 || (stripPipe && c == '|')) ? ' ' : c;
  }
  return n;
}

// --- Streaming contexts ---------------------------------------------------

struct CountCtx {
  const std::string* dropWord;  // nullptr = count every line
  int count;
  bool dupSeen;
  char savedExcerpt[FlashcardDeck::EXCERPT_MAX];  // excerpt of the matched dropWord line
  int savedExcerptLen;
  char savedChapter[FlashcardDeck::CHAPTER_MAX];  // chapter of the matched dropWord line
  int savedChapterLen;
};

bool countLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<CountCtx*>(ctx);
  if (c->dropWord && lineWordEquals(line, len, *c->dropWord)) {
    c->dupSeen = true;
    const Parsed p = parseLine(line, len);
    c->savedExcerptLen = std::min(p.excerptLen, FlashcardDeck::EXCERPT_MAX);
    if (c->savedExcerptLen > 0) memcpy(c->savedExcerpt, p.excerpt, static_cast<size_t>(c->savedExcerptLen));
    c->savedChapterLen = std::min(p.chapterLen, FlashcardDeck::CHAPTER_MAX);
    if (c->savedChapterLen > 0) memcpy(c->savedChapter, p.chapter, static_cast<size_t>(c->savedChapterLen));
  } else {
    c->count++;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// forEachLine (512-byte buffer; lines carry an excerpt)
// ---------------------------------------------------------------------------

bool FlashcardDeck::forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len), void* ctx) {
  HalFile file;
  if (!Storage.openFileForRead("FCD", path, file)) return false;

  char lineBuf[512];
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

    if (lineLen < static_cast<int>(sizeof(lineBuf)) - 1) lineBuf[lineLen++] = static_cast<char>(b);
  }

  if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    fn(ctx, lineBuf, lineLen);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Leitner core
// ---------------------------------------------------------------------------

void FlashcardDeck::applyGrade(uint8_t& box, uint32_t& dueDay, bool correct, uint32_t today) {
  if (!correct) {
    box = 0;
  } else if (box >= TOP_BOX) {
    box = RETIRED;  // graduated -- 2 stable reps passed; leave dueDay as-is
    return;
  } else {
    box++;
  }
  dueDay = today + BOX_INTERVAL_DAYS[box];
}

bool FlashcardDeck::isDue(uint8_t box, uint32_t dueDay, uint32_t today) {
  return box != RETIRED && (dueDay == 0 || dueDay <= today);
}

// ---------------------------------------------------------------------------
// enroll
// ---------------------------------------------------------------------------

bool FlashcardDeck::enroll(const std::string& cachePath, const std::string& word, const std::string& excerpt,
                           const std::string& chapter) {
  if (word.empty() || cachePath.empty()) return false;

  const std::string path = filePath(cachePath);

  // Pass 1: count survivors and, if the word is already present, capture its
  // existing excerpt + chapter so we can preserve them when the new ones are empty.
  CountCtx cc{&word, 0, false, {}, 0, {}, 0};
  forEachLine(path, countLine, &cc);

  char newExcerpt[EXCERPT_MAX];
  int newLen = sanitizeField(excerpt, newExcerpt, EXCERPT_MAX, /*stripPipe=*/false);
  // Empty new excerpt on a re-enroll keeps the previously captured sentence.
  const char* useExcerpt = newExcerpt;
  if (newLen == 0 && cc.dupSeen) {
    useExcerpt = cc.savedExcerpt;
    newLen = cc.savedExcerptLen;
  }

  char newChapter[CHAPTER_MAX];
  int chapLen = sanitizeField(chapter, newChapter, CHAPTER_MAX, /*stripPipe=*/true);
  const char* useChapter = newChapter;
  if (chapLen == 0 && cc.dupSeen) {
    useChapter = cc.savedChapter;
    chapLen = cc.savedChapterLen;
  }

  // Fast path: brand-new word -> a single append. Prior cards are untouched.
  if (!cc.dupSeen) {
    HalFile out;
    if (!Storage.openFileForAppend("FCD", path.c_str(), out)) {
      LOG_ERR("FCD", "Failed to open for append: %s", path.c_str());
      return false;
    }
    const bool ok = writeCard(out, word.c_str(), word.size(), 0, 0, useChapter, chapLen, useExcerpt, newLen);
    out.close();
    if (!ok) LOG_ERR("FCD", "Enroll append failed: %s", path.c_str());
    return ok;
  }

  // Dedup path: rewrite survivors to a temp file (dropping the old copy), then
  // append the refreshed card as newest. The original is replaced only after a
  // clean write so a mid-write failure cannot lose the deck.
  const std::string tmpPath = tmpFilePath(cachePath);
  HalFile out;
  if (!Storage.openFileForWrite("FCD", tmpPath, out)) {
    LOG_ERR("FCD", "Failed to open temp for write: %s", tmpPath.c_str());
    return false;
  }
  struct CopyCtx {
    const std::string* word;
    HalFile* out;
    bool ok;
  } wc{&word, &out, true};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<CopyCtx*>(ctx);
        if (lineWordEquals(line, len, *c->word)) return true;  // drop old copy
        c->ok = writeRaw(*c->out, line, len) && c->out->write("\n", 1) == 1;
        return c->ok;
      },
      &wc);
  wc.ok = wc.ok && writeCard(out, word.c_str(), word.size(), 0, 0, useChapter, chapLen, useExcerpt, newLen);
  out.close();

  if (!wc.ok) {
    LOG_ERR("FCD", "Enroll write failed: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(path.c_str());
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("FCD", "Enroll rename failed: %s", path.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// count / loadWindow
// ---------------------------------------------------------------------------

int FlashcardDeck::count(const std::string& cachePath) {
  CountCtx cc{nullptr, 0, false, {}, 0, {}, 0};
  forEachLine(filePath(cachePath), countLine, &cc);
  return cc.count;
}

FlashcardDeck::Stats FlashcardDeck::computeStats(const std::string& cachePath, uint32_t today) {
  struct StatsCtx {
    Stats s;
    uint32_t today;
  } sc{{}, today};
  forEachLine(
      filePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<StatsCtx*>(ctx);
        const Parsed p = parseLine(line, len);
        c->s.total++;
        if (p.box == RETIRED) {
          c->s.mastered++;
        } else {
          c->s.boxHist[p.box <= TOP_BOX ? p.box : TOP_BOX]++;
          if (isDue(p.box, p.dueDay, c->today)) {
            c->s.due++;
          } else if (c->s.nextDueDay == 0 || p.dueDay < c->s.nextDueDay) {
            c->s.nextDueDay = p.dueDay;  // soonest future scheduled day
          }
        }
        return true;
      },
      &sc);
  return sc.s;
}

int FlashcardDeck::loadWindow(const std::string& cachePath, int startNewest, int n, Entry* out) {
  if (startNewest < 0 || n <= 0 || !out) return 0;
  const std::string path = filePath(cachePath);

  CountCtx cc{nullptr, 0, false, {}, 0, {}, 0};
  if (!forEachLine(path, countLine, &cc) || cc.count == 0) return 0;
  const int total = cc.count;
  if (startNewest >= total) return 0;

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
        const Parsed p = parseLine(line, len);
        Entry& e = c->out[c->hi - idx];  // newest-first offset
        e.word.assign(line, static_cast<size_t>(p.wordLen));
        e.box = p.box;
        e.dueDay = p.dueDay;
        e.chapter.assign(p.chapter, static_cast<size_t>(p.chapterLen));
        e.excerpt.assign(p.excerpt, static_cast<size_t>(p.excerptLen));
        return c->fileIdx <= c->hi;  // stop once past the window
      },
      &wc);
  return want;
}

// ---------------------------------------------------------------------------
// removeAt
// ---------------------------------------------------------------------------

bool FlashcardDeck::removeAt(const std::string& cachePath, int index) {
  if (index < 0) return false;
  const std::string path = filePath(cachePath);
  const std::string tmpPath = tmpFilePath(cachePath);

  CountCtx cc{nullptr, 0, false, {}, 0, {}, 0};
  if (!forEachLine(path, countLine, &cc)) return false;
  if (index >= cc.count) return false;

  HalFile out;
  if (!Storage.openFileForWrite("FCD", tmpPath, out)) {
    LOG_ERR("FCD", "Failed to open temp for write: %s", tmpPath.c_str());
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
        if (c->seen++ == c->skipIdx) return true;  // drop this row
        c->ok = writeRaw(*c->out, line, len) && c->out->write("\n", 1) == 1;
        return c->ok;
      },
      &wc);
  out.close();

  if (!wc.ok) {
    LOG_ERR("FCD", "Remove write failed: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(path.c_str());
  return Storage.rename(tmpPath.c_str(), path.c_str());
}

// ---------------------------------------------------------------------------
// grade
// ---------------------------------------------------------------------------

bool FlashcardDeck::grade(const std::string& cachePath, const std::string& word, bool correct, uint32_t today) {
  if (word.empty() || cachePath.empty()) return false;
  const std::string path = filePath(cachePath);

  // Scan first: skip the rewrite entirely if the word is absent.
  CountCtx cc{&word, 0, false, {}, 0, {}, 0};
  if (!forEachLine(path, countLine, &cc) || !cc.dupSeen) return false;

  const std::string tmpPath = tmpFilePath(cachePath);
  HalFile out;
  if (!Storage.openFileForWrite("FCD", tmpPath, out)) {
    LOG_ERR("FCD", "Failed to open temp for write: %s", tmpPath.c_str());
    return false;
  }
  struct GradeCtx {
    const std::string* word;
    HalFile* out;
    bool correct;
    uint32_t today;
    bool ok;
  } gc{&word, &out, correct, today, true};
  forEachLine(
      path,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<GradeCtx*>(ctx);
        const Parsed p = parseLine(line, len);
        if (static_cast<size_t>(p.wordLen) != c->word->size() || memcmp(line, c->word->c_str(), p.wordLen) != 0) {
          c->ok = writeRaw(*c->out, line, len) && c->out->write("\n", 1) == 1;  // copy verbatim
          return c->ok;
        }
        uint8_t box = p.box;
        uint32_t dueDay = p.dueDay;
        applyGrade(box, dueDay, c->correct, c->today);
        c->ok = writeCard(*c->out, line, static_cast<size_t>(p.wordLen), box, dueDay, p.chapter, p.chapterLen,
                          p.excerpt, p.excerptLen);
        return c->ok;
      },
      &gc);
  out.close();

  if (!gc.ok) {
    LOG_ERR("FCD", "Grade write failed: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(path.c_str());
  return Storage.rename(tmpPath.c_str(), path.c_str());
}

// ---------------------------------------------------------------------------
// buildSession
// ---------------------------------------------------------------------------

namespace {

enum class Tier { ScheduledDue, New, NonRetired };

bool qualifies(Tier t, uint8_t box, uint32_t dueDay, uint32_t today) {
  if (box == FlashcardDeck::RETIRED) return false;
  switch (t) {
    case Tier::ScheduledDue:
      return dueDay != 0 && dueDay <= today;
    case Tier::New:
      return dueDay == 0;
    case Tier::NonRetired:
      return true;
  }
  return false;
}

// Collect the newest `cap` qualifying cards' newest-first indices into out,
// using a fixed ring of size cap over the oldest-first stream (no per-deck
// allocation). Returns the count written (<= cap). `today` and the tier select
// the predicate; `total` converts file index -> newest-first index.
struct SelectCtx {
  Tier tier;
  uint32_t today;
  int total;
  int fileIdx;
  uint16_t* ring;  // size cap
  int cap;
  int seen;  // qualifying count so far
};

bool selectLine(void* ctx, const char* line, int len) {
  auto* c = static_cast<SelectCtx*>(ctx);
  const int fileIdx = c->fileIdx++;
  const Parsed p = parseLine(line, len);
  if (!qualifies(c->tier, p.box, p.dueDay, c->today)) return true;
  const uint16_t newestIdx = static_cast<uint16_t>(c->total - 1 - fileIdx);
  c->ring[c->seen % c->cap] = newestIdx;  // keep newest cap (overwrites oldest)
  c->seen++;
  return true;
}

// Session cap is a small constant by contract (review sessions are 20-50
// cards); a fixed ring bounds RAM regardless of deck size.
constexpr int SESSION_RING_MAX = 64;

// Run a tier selection pass over `path`, appending up to (cap-written) results
// to out (newest-first). Returns the new total written.
int selectTier(const std::string& path, Tier tier, uint32_t today, int total, uint16_t* out, int written, int cap) {
  int budget = cap - written;
  if (budget <= 0) return written;
  if (budget > SESSION_RING_MAX) budget = SESSION_RING_MAX;

  uint16_t ring[SESSION_RING_MAX];
  SelectCtx sc{tier, today, total, 0, ring, budget, 0};
  FlashcardDeck::forEachLine(path, selectLine, &sc);

  // The ring retains the newest min(seen,budget) qualifying cards. Emit them
  // newest-first: the most recently written slot holds the smallest newestIdx.
  const int m = std::min(sc.seen, budget);
  for (int k = 0; k < m; k++) out[written + k] = ring[(sc.seen - 1 - k) % budget];
  return written + m;
}

}  // namespace

int FlashcardDeck::buildSession(const std::string& cachePath, SessionScope scope, uint32_t today, int cap,
                                uint16_t* out) {
  if (cap <= 0 || !out) return 0;
  const std::string path = filePath(cachePath);

  const int total = count(cachePath);
  if (total == 0) return 0;

  // Clock unavailable -> due ordering is meaningless; fall back to all-shuffled.
  const bool dueFirst = (scope == SessionScope::DueFirst) && today > 0;

  int written = 0;
  if (dueFirst) {
    written = selectTier(path, Tier::ScheduledDue, today, total, out, written, cap);
    written = selectTier(path, Tier::New, today, total, out, written, cap);
  } else {
    written = selectTier(path, Tier::NonRetired, today, total, out, written, cap);
  }
  return written;
}
