#include "FlashcardDeck.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SdDebugLog.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DictStopwords.h"

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::string FlashcardDeck::filePath(const std::string& cachePath) { return cachePath + "/" + FILE_NAME; }

std::string FlashcardDeck::tmpFilePath(const std::string& cachePath) {
  return cachePath + "/dictionary_flashcards.tmp";
}

std::string FlashcardDeck::verFilePath(const std::string& cachePath) {
  return cachePath + "/dictionary_flashcards.ver";
}

std::string FlashcardDeck::tombFilePath(const std::string& cachePath) {
  return cachePath + "/dictionary_flashcards.tomb";
}

std::string FlashcardDeck::syncFilePath(const std::string& cachePath) {
  return cachePath + "/dictionary_flashcards.sync";
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

// True iff [p, p+n) is a non-empty run of decimal digits. Used to tell a new
// inline version field apart from a legacy excerpt's leading token.
bool isAllDigits(const char* p, int n) {
  if (n <= 0) return false;
  for (int i = 0; i < n; i++)
    if (p[i] < '0' || p[i] > '9') return false;
  return true;
}

// Parsed fields of a "word|box|dueDay|chapter|version|count|dictHash|excerpt" line. The excerpt
// is the remainder after the last delimiter, so an embedded '|' in the excerpt is harmless to
// the fields BEFORE it. Each optional trailing field (version, count, dictHash) is recognised
// only when it is all-digits; the first non-numeric field ends the header and starts the
// excerpt. That is how every older layout still parses:
//   word|box|dueDay|chapter|version|count|excerpt   -> dictHash 0
//   word|box|dueDay|chapter|version|excerpt         -> count 1, dictHash 0
//   word|box|dueDay|chapter|excerpt                 -> version 0, count 1, dictHash 0
//   word|box|dueDay|excerpt                         -> no chapter either
// Malformed lines (fewer than three '|') degrade to box 0 / dueDay 0 / no chapter / version 0 /
// no excerpt, whole line = word.
//
// The all-digits rule is a heuristic only for lines this build did NOT write. writeCard always
// emits all three optional fields, 0 included, so a current line has exactly seven header
// delimiters, every sniff tests a numeric field, and the excerpt (which may contain '|') starts
// past the last delimiter parseLine tracks. A LEGACY line that is short a field AND whose
// excerpt begins with a numeric pipe-token ("1999|was a good year") reads that token as the
// missing field. That hazard predates dictHash — it already applied to count — and it cannot be
// fixed from the write end for lines already on disk, so it is pinned by a test rather than
// papered over. Adding a FOURTH optional field would need the same audit again.
struct Parsed {
  int wordLen;
  uint8_t box;
  uint32_t dueDay;
  const char* chapter;
  int chapterLen;
  uint32_t version;
  uint32_t count;     // local lookup count; defaults to 1 when the field is absent (legacy)
  uint32_t dictHash;  // DictionaryRegistry::nameHash of the dictionary the card was saved from;
                      // 0 = unrecorded (legacy line, or a card received from an older device)
  const char* excerpt;
  int excerptLen;
};

Parsed parseLine(const char* line, int len) {
  Parsed r{len, 0, 0, line + len, 0, 0, 1, 0, line + len, 0};
  int p0 = -1, p1 = -1, p2 = -1, p3 = -1, p4 = -1, p5 = -1, p6 = -1;
  for (int i = 0; i < len; i++) {
    if (line[i] != '|') continue;
    if (p0 < 0)
      p0 = i;
    else if (p1 < 0)
      p1 = i;
    else if (p2 < 0)
      p2 = i;
    else if (p3 < 0)
      p3 = i;
    else if (p4 < 0)
      p4 = i;
    else if (p5 < 0)
      p5 = i;
    else {
      p6 = i;
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
    return r;
  }
  r.chapter = line + p2 + 1;
  r.chapterLen = p3 - (p2 + 1);
  if (p4 >= 0 && isAllDigits(line + p3 + 1, p4 - (p3 + 1))) {
    // chapter|version|... : version present.
    r.version = parseU32(line + p3 + 1, p4 - (p3 + 1));
    if (p5 >= 0 && isAllDigits(line + p4 + 1, p5 - (p4 + 1))) {
      // chapter|version|count|... : count present.
      r.count = parseU32(line + p4 + 1, p5 - (p4 + 1));
      if (p6 >= 0 && isAllDigits(line + p5 + 1, p6 - (p5 + 1))) {
        // Current 8-field line: chapter|version|count|dictHash|excerpt.
        r.dictHash = parseU32(line + p5 + 1, p6 - (p5 + 1));
        r.excerpt = line + p6 + 1;
        r.excerptLen = len - (p6 + 1);
      } else {
        // 7-field line: chapter|version|count|excerpt (dictHash absent -> 0).
        r.excerpt = line + p5 + 1;
        r.excerptLen = len - (p5 + 1);
      }
    } else {
      // 6-field line: chapter|version|excerpt (count absent -> default 1).
      r.excerpt = line + p4 + 1;
      r.excerptLen = len - (p4 + 1);
    }
  } else {
    // Legacy 5-field line (or excerpt with a leading non-numeric token): the
    // remainder after chapter is the excerpt, version stays 0 and count 1.
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

// Write a full "word|box|dueDay|chapter|version|count|dictHash|excerpt\n" card line.
bool writeCard(HalFile& out, const char* word, size_t wordLen, uint8_t box, uint32_t dueDay, const char* chapter,
               int chapterLen, uint32_t version, uint32_t count, uint32_t dictHash, const char* excerpt,
               int excerptLen) {
  char mid[24];
  const int m = snprintf(mid, sizeof(mid), "|%u|%lu|", static_cast<unsigned>(box), static_cast<unsigned long>(dueDay));
  bool ok = writeRaw(out, word, static_cast<int>(wordLen)) && writeRaw(out, mid, m);
  if (ok && chapterLen > 0) ok = writeRaw(out, chapter, chapterLen);
  // |version|count|dictHash| (delimits chapter from the remainder excerpt). Of these only
  // version goes on the sync wire in the card line — buildCardPayload omits count (a local
  // lookup tally) and dictHash (carried by its own 'D' line, so older devices ignore it
  // instead of swallowing it into the excerpt).
  //
  // dictHash is written unconditionally, 0 included: a fixed field count is what lets the
  // all-digits sniffing in parseLine stay unambiguous for every line this build produces.
  char vtail[36];
  const int v = snprintf(vtail, sizeof(vtail), "|%lu|%lu|%lu|", static_cast<unsigned long>(version),
                         static_cast<unsigned long>(count), static_cast<unsigned long>(dictHash));
  if (ok) ok = writeRaw(out, vtail, v);
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
  uint32_t savedCount;     // lookup count of the matched dropWord line (for the re-enroll bump)
  uint32_t savedDictHash;  // dictionary of the matched dropWord line (kept when the re-enroll
                           // supplies none, e.g. a re-lookup from the history list)
  uint8_t savedBox;        // Leitner box + schedule of the matched dropWord line. Carried over
  uint32_t savedDueDay;    // for the same reason updateRemoteCard keeps them when the identical
                           // re-enroll arrives from a peer: looking a word up again is not a
                           // failed recall (enrollment is automatic on every lookup), and the
                           // deliberate signal for that is grading it wrong in review.
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
    c->savedCount = p.count;
    c->savedDictHash = p.dictHash;
    c->savedBox = p.box;
    c->savedDueDay = p.dueDay;
  } else {
    c->count++;
  }
  return true;
}

// Recently-enrolled words, for the re-count throttle. Fixed static storage — 8 slots x 8
// bytes — so the lookup path allocates nothing; the deck itself is streamed line-by-line
// for the same reason (see the class comment). Oldest slot is overwritten, which is
// exactly right: an entry that falls out simply stops suppressing.
//
// The key covers the BOOK as well as the word. Without it, looking a word up in book A
// and then in book B within the window would suppress B's enroll and leave B's deck
// without the card at all.
constexpr int ENROLL_RING_SLOTS = 8;

struct EnrollStamp {
  uint32_t key = 0;      // 0 = empty slot
  uint32_t stampMs = 0;  // millis() of the enroll that filled it
};

EnrollStamp enrollRing[ENROLL_RING_SLOTS];
int enrollRingNext = 0;

uint32_t enrollKey(const std::string& cachePath, const std::string& word) {
  uint32_t h = 2166136261u;  // FNV-1a over cachePath + '\0' + word
  for (const char c : cachePath) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  h = (h ^ 0u) * 16777619u;
  for (const char c : word) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  return h == 0 ? 1u : h;  // 0 marks an empty slot
}

// True when this word was enrolled less than windowMs ago. Unsigned subtraction, so the
// millis() wrap at ~49.7 days reads as "long ago" rather than "just now".
bool enrollThrottled(uint32_t key, uint32_t nowMs, uint32_t windowMs) {
  if (windowMs == 0) return false;
  for (const auto& slot : enrollRing) {
    if (slot.key == key) return nowMs - slot.stampMs < windowMs;
  }
  return false;
}

// Start this word's window. Called only after a card was actually written, so an enroll that
// failed on SD is retried by the next lookup instead of being throttled against a card that
// was never created.
void recordEnroll(uint32_t key, uint32_t nowMs, uint32_t windowMs) {
  if (windowMs == 0) return;  // no window configured: leave the ring alone
  for (auto& slot : enrollRing) {
    if (slot.key == key) {
      slot.stampMs = nowMs;
      return;
    }
  }
  enrollRing[enrollRingNext] = EnrollStamp{key, nowMs};
  enrollRingNext = (enrollRingNext + 1) % ENROLL_RING_SLOTS;
}

}  // namespace

void FlashcardDeck::clearEnrollCooldown() {
  for (auto& slot : enrollRing) slot = EnrollStamp{};
  enrollRingNext = 0;
}

// ---------------------------------------------------------------------------
// forEachLine (512-byte buffer; lines carry an excerpt)
// ---------------------------------------------------------------------------

bool FlashcardDeck::forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len), void* ctx) {
  HalFile file;
  if (!Storage.openFileForRead("FCD", path, file)) return false;

  char lineBuf[512];
  int lineLen = 0;

  // Block reads, not byte reads — same change and same reason as
  // LookupHistory::forEachLine (see the comment there): each HalFile call takes the
  // storage mutex, and mergeBlob rescans the deck once per blob line. Behaviour is
  // identical; only the byte source changes.
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

// ---------------------------------------------------------------------------
// rewriteDeck (shared atomic temp-file -> rename scaffold)
// ---------------------------------------------------------------------------

namespace {

// Bridges a (ctx, out, line, len) transform to forEachLine's signature, carrying
// the output file and a sticky ok flag so a single I/O failure stops the stream.
struct RewriteAdapter {
  bool (*lineFn)(void* ctx, HalFile& out, const char* line, int len);
  void* ctx;
  HalFile* out;
  bool ok;
};

bool rewriteAdapterLine(void* a, const char* line, int len) {
  auto* r = static_cast<RewriteAdapter*>(a);
  r->ok = r->lineFn(r->ctx, *r->out, line, len);
  return r->ok;  // false -> forEachLine stops early on I/O failure
}

}  // namespace

bool FlashcardDeck::rewriteDeck(const std::string& cachePath, void* ctx,
                                bool (*lineFn)(void* ctx, HalFile& out, const char* line, int len),
                                bool (*tailFn)(void* ctx, HalFile& out)) {
  const std::string path = filePath(cachePath);
  const std::string tmpPath = tmpFilePath(cachePath);

  HalFile out;
  if (!Storage.openFileForWrite("FCD", tmpPath, out)) {
    LOG_ERR("FCD", "Failed to open temp for write: %s", tmpPath.c_str());
    return false;
  }
  RewriteAdapter ad{lineFn, ctx, &out, true};
  forEachLine(path, rewriteAdapterLine, &ad);
  if (ad.ok && tailFn) ad.ok = tailFn(ctx, out);
  out.close();

  if (!ad.ok) {
    LOG_ERR("FCD", "Deck rewrite failed: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(path.c_str());
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("FCD", "Deck rewrite rename failed: %s", path.c_str());
    return false;
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
  return box != RETIRED && box != SUSPENDED && (dueDay == 0 || dueDay <= today);
}

// ---------------------------------------------------------------------------
// enroll
// ---------------------------------------------------------------------------

bool FlashcardDeck::enroll(const std::string& cachePath, const std::string& word, const std::string& excerpt,
                           const std::string& chapter, uint32_t dictHash, uint32_t nowMs, uint32_t windowMs) {
  if (word.empty() || cachePath.empty()) return false;
  // Same filter, same reason, as LookupHistory::addWordIf (LookupHistory.cpp:362): a closed-class
  // function word is not study material. Enrollment is automatic on every in-book lookup, so
  // without this every "a"/"the"/"of" the reader taps became a card while being correctly kept
  // out of the history log — the two lists disagreeing about the same lookup.
  //
  // Ahead of the throttle deliberately: a word that can never be enrolled should not occupy a
  // slot in the re-enroll table either.
  if (DictStopwords::isStopword(word)) return false;

  // Before any file I/O: a throttled re-enroll skips the counting pass AND the full deck
  // rewrite below, which is the expensive half of looking a word up twice in one sitting.
  const uint32_t throttleKey = enrollKey(cachePath, word);
  if (enrollThrottled(throttleKey, nowMs, windowMs)) return true;

  const std::string path = filePath(cachePath);

  // Pass 1: count survivors and, if the word is already present, capture its
  // existing excerpt + chapter so we can preserve them when the new ones are empty.
  CountCtx cc{&word, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
  forEachLine(path, countLine, &cc);

  // stripPipe stays FALSE for the excerpt (unlike chapter). The excerpt is the line remainder
  // and writeCard always emits all three optional fields — version|count|dictHash, 0 included —
  // so a line this build writes always has exactly seven header delimiters and parseLine's
  // all-digits sniffs always resolve against numeric fields. Any '|' in the excerpt lands past
  // the last one parseLine tracks and is returned intact. Stripping it would destroy the user's
  // sentence to buy a disambiguation the fixed field count already provides.
  char newExcerpt[EXCERPT_MAX];
  int newLen = sanitizeField(excerpt, newExcerpt, EXCERPT_MAX, /*stripPipe=*/false);
  // Empty new excerpt on a re-enroll keeps the previously captured sentence.
  const char* useExcerpt = newExcerpt;
  if (newLen == 0 && cc.dupSeen) {
    useExcerpt = cc.savedExcerpt;
    newLen = cc.savedExcerptLen;
  }

  // Same rule as excerpt/chapter: a re-enroll that carries no dictionary context (the history
  // list re-lookup) must not erase the association the original in-book lookup recorded.
  if (dictHash == 0 && cc.dupSeen) dictHash = cc.savedDictHash;

  char newChapter[CHAPTER_MAX];
  int chapLen = sanitizeField(chapter, newChapter, CHAPTER_MAX, /*stripPipe=*/true);
  const char* useChapter = newChapter;
  if (chapLen == 0 && cc.dupSeen) {
    useChapter = cc.savedChapter;
    chapLen = cc.savedChapterLen;
  }

  // Enroll is new content to propagate: stamp a fresh Lamport version and drop
  // any tombstone for this word so a stale remote delete can't resurrect over
  // this newer add on merge (mirrors LookupHistory::addWordVer).
  const uint32_t version = nextVersion(cachePath);
  clearTombstone(cachePath, word);

  // Lookup count: a brand-new card is 1; a re-enroll bumps the saved count. Local
  // only -- never synced (a received card starts at 1 on the peer).
  const uint32_t newCount = cc.dupSeen ? cc.savedCount + 1 : 1;

  // Fast path: brand-new word -> a single append. Prior cards are untouched.
  if (!cc.dupSeen) {
    HalFile out;
    if (!Storage.openFileForAppend("FCD", path.c_str(), out)) {
      LOG_ERR("FCD", "Failed to open for append: %s", path.c_str());
      return false;
    }
    const bool ok = writeCard(out, word.c_str(), word.size(), 0, 0, useChapter, chapLen, version, newCount, dictHash,
                              useExcerpt, newLen);
    out.close();
    if (!ok) LOG_ERR("FCD", "Enroll append failed: %s", path.c_str());
    if (ok) recordEnroll(throttleKey, nowMs, windowMs);
    return ok;
  }

  // Dedup path: drop the old copy while copying survivors, then append the
  // refreshed card as newest -- via the shared atomic rewrite.
  struct EnrollCtx {
    const std::string* word;
    const char* chapter;
    int chapLen;
    const char* excerpt;
    int excerptLen;
    uint32_t version;
    uint32_t count;
    uint32_t dictHash;
    uint8_t box;
    uint32_t dueDay;
  } ec{&word, useChapter, chapLen, useExcerpt, newLen, version, newCount, dictHash, cc.savedBox, cc.savedDueDay};
  const bool ok = rewriteDeck(
      cachePath, &ec,
      [](void* ctx, HalFile& out, const char* line, int len) {
        auto* c = static_cast<EnrollCtx*>(ctx);
        if (lineWordEquals(line, len, *c->word)) return true;  // drop old copy
        return writeRaw(out, line, len) && out.write("\n", 1) == 1;
      },
      [](void* ctx, HalFile& out) {
        auto* c = static_cast<EnrollCtx*>(ctx);
        // c->box / c->dueDay, not 0 / 0: the card keeps its place in the review schedule.
        return writeCard(out, c->word->c_str(), c->word->size(), c->box, c->dueDay, c->chapter, c->chapLen, c->version,
                         c->count, c->dictHash, c->excerpt, c->excerptLen);
      });
  if (ok) recordEnroll(throttleKey, nowMs, windowMs);
  return ok;
}

// ---------------------------------------------------------------------------
// count / loadWindow
// ---------------------------------------------------------------------------

int FlashcardDeck::count(const std::string& cachePath) {
  CountCtx cc{nullptr, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
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
        } else if (p.box == SUSPENDED) {
          c->s.suspended++;
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

bool FlashcardDeck::hasDueCards(const std::string& cachePath, uint32_t today) {
  struct DueCtx {
    uint32_t today;
    bool found;
  } dc{today, false};
  forEachLine(
      filePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<DueCtx*>(ctx);
        const Parsed p = parseLine(line, len);
        if (!isDue(p.box, p.dueDay, c->today)) return true;
        c->found = true;
        return false;  // stop the scan at the first due card
      },
      &dc);
  return dc.found;
}

int FlashcardDeck::loadWindow(const std::string& cachePath, int startNewest, int n, Entry* out, bool wordsOnly) {
  if (startNewest < 0 || n <= 0 || !out) return 0;
  const std::string path = filePath(cachePath);

  CountCtx cc{nullptr, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
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
    bool wordsOnly;
  } wc{0, lo, hi, out, wordsOnly};
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
        e.version = p.version;
        e.count = p.count;  // kept even in wordsOnly mode so the list row can show "xN"
        // Also kept in wordsOnly mode: the list view opens definitions straight from a row, so
        // it needs the association without a second pass over the deck.
        e.dictHash = p.dictHash;
        if (c->wordsOnly) {
          // List view shows word + box glyph only; skip the two string allocs.
          e.chapter.clear();
          e.excerpt.clear();
        } else {
          e.chapter.assign(p.chapter, static_cast<size_t>(p.chapterLen));
          e.excerpt.assign(p.excerpt, static_cast<size_t>(p.excerptLen));
        }
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
  // The card is going away, so nothing is left for the re-count window to protect: a
  // re-lookup right after a delete must re-create the card, not be throttled against it.
  clearEnrollCooldown();
  const std::string path = filePath(cachePath);

  CountCtx cc{nullptr, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
  if (!forEachLine(path, countLine, &cc)) return false;
  if (index >= cc.count) return false;

  struct RemoveCtx {
    int seen;
    int skipIdx;
    std::string word;  // captured word of the dropped row (for the tombstone)
  } rc{0, index, {}};
  const bool ok = rewriteDeck(cachePath, &rc, [](void* ctx, HalFile& out, const char* line, int len) {
    auto* c = static_cast<RemoveCtx*>(ctx);
    if (c->seen++ == c->skipIdx) {
      const Parsed p = parseLine(line, len);
      c->word.assign(line, static_cast<size_t>(p.wordLen));
      return true;  // drop this row
    }
    return writeRaw(out, line, len) && out.write("\n", 1) == 1;
  });
  // Tombstone the dropped word so the delete propagates on sync.
  if (ok && !rc.word.empty()) setTombstone(cachePath, rc.word, nextVersion(cachePath));
  return ok;
}

bool FlashcardDeck::remove(const std::string& cachePath, const std::string& word) {
  if (word.empty() || cachePath.empty()) return false;
  clearEnrollCooldown();  // see removeAt()
  const std::string path = filePath(cachePath);

  // Scan first: skip the rewrite entirely if the word is absent.
  CountCtx cc{&word, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
  if (!forEachLine(path, countLine, &cc) || !cc.dupSeen) return false;

  const bool ok =
      rewriteDeck(cachePath, const_cast<std::string*>(&word), [](void* ctx, HalFile& out, const char* line, int len) {
        const auto* w = static_cast<const std::string*>(ctx);
        if (lineWordEquals(line, len, *w)) return true;  // drop the matching row
        return writeRaw(out, line, len) && out.write("\n", 1) == 1;
      });
  // Tombstone the dropped word so the delete propagates on sync.
  if (ok) setTombstone(cachePath, word, nextVersion(cachePath));
  return ok;
}

// ---------------------------------------------------------------------------
// grade
// ---------------------------------------------------------------------------

bool FlashcardDeck::grade(const std::string& cachePath, const std::string& word, bool correct, uint32_t today) {
  if (word.empty() || cachePath.empty()) return false;
  const std::string path = filePath(cachePath);

  // Scan first: skip the rewrite entirely if the word is absent.
  CountCtx cc{&word, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
  if (!forEachLine(path, countLine, &cc) || !cc.dupSeen) return false;

  struct GradeCtx {
    const std::string* word;
    bool correct;
    uint32_t today;
  } gc{&word, correct, today};
  return rewriteDeck(cachePath, &gc, [](void* ctx, HalFile& out, const char* line, int len) {
    auto* c = static_cast<GradeCtx*>(ctx);
    const Parsed p = parseLine(line, len);
    if (static_cast<size_t>(p.wordLen) != c->word->size() || memcmp(line, c->word->c_str(), p.wordLen) != 0)
      return writeRaw(out, line, len) && out.write("\n", 1) == 1;  // copy verbatim
    uint8_t box = p.box;
    uint32_t dueDay = p.dueDay;
    applyGrade(box, dueDay, c->correct, c->today);
    // Grading is a local schedule change -- preserve the wire version, lookup count and the
    // dictionary the card was saved from.
    return writeCard(out, line, static_cast<size_t>(p.wordLen), box, dueDay, p.chapter, p.chapterLen, p.version,
                     p.count, p.dictHash, p.excerpt, p.excerptLen);
  });
}

// ---------------------------------------------------------------------------
// suspend / unsuspend  (fixed-value row rewrite over the shared rewriteDeck
// scaffold; no deck materialization, no new heap)
// ---------------------------------------------------------------------------

bool FlashcardDeck::suspend(const std::string& cachePath, const std::string& word) {
  return setBoxForWord(cachePath, word, SUSPENDED, 0);
}

bool FlashcardDeck::unsuspend(const std::string& cachePath, const std::string& word) {
  return setBoxForWord(cachePath, word, 0, 0);
}

bool FlashcardDeck::setBoxForWord(const std::string& cachePath, const std::string& word, uint8_t box, uint32_t dueDay) {
  if (word.empty() || cachePath.empty()) return false;
  const std::string path = filePath(cachePath);

  // Scan first: skip the rewrite entirely if the word is absent.
  CountCtx cc{&word, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
  if (!forEachLine(path, countLine, &cc) || !cc.dupSeen) return false;

  // Force the matched row's box/dueDay, copying every other line verbatim.
  struct SetCtx {
    const std::string* word;
    uint8_t box;
    uint32_t dueDay;
  } sc{&word, box, dueDay};
  return rewriteDeck(cachePath, &sc, [](void* ctx, HalFile& out, const char* line, int len) {
    auto* c = static_cast<SetCtx*>(ctx);
    const Parsed p = parseLine(line, len);
    if (static_cast<size_t>(p.wordLen) != c->word->size() || memcmp(line, c->word->c_str(), p.wordLen) != 0)
      return writeRaw(out, line, len) && out.write("\n", 1) == 1;  // copy verbatim
    // suspend/unsuspend are local schedule changes -- preserve version, count and dictHash.
    return writeCard(out, line, static_cast<size_t>(p.wordLen), c->box, c->dueDay, p.chapter, p.chapterLen, p.version,
                     p.count, p.dictHash, p.excerpt, p.excerptLen);
  });
}

// ---------------------------------------------------------------------------
// buildSession
// ---------------------------------------------------------------------------

namespace {

enum class Tier { ScheduledDue, New, NonRetired, Suspended };

bool qualifies(Tier t, uint8_t box, uint32_t dueDay, uint32_t today) {
  // The Suspended tier deliberately selects only suspended cards; every other
  // tier excludes both retired and suspended cards.
  if (t == Tier::Suspended) return box == FlashcardDeck::SUSPENDED;
  if (box == FlashcardDeck::RETIRED || box == FlashcardDeck::SUSPENDED) return false;
  switch (t) {
    case Tier::ScheduledDue:
      return dueDay != 0 && dueDay <= today;
    case Tier::New:
      return dueDay == 0;
    case Tier::NonRetired:
      return true;
    case Tier::Suspended:
      return false;  // handled above
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

  // Suspended scope: a single pass over set-aside cards (no due ordering).
  if (scope == SessionScope::Suspended) {
    return selectTier(path, Tier::Suspended, today, total, out, 0, cap);
  }

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

// ===========================================================================
// Cross-device sync ("fc" blob) -- mirrors LookupHistory's Lamport-versioned
// delta sync, with the all-at-once keyframe replaced by a rolling cursor so the
// per-sync blob is bounded by a card count, not deck size.
// ===========================================================================

namespace {

// Parse a tombstone line "word|VER": returns the version, sets *wordLen to the
// word length (the bytes before the last '|'). A line with no '|' is treated as
// a bare word at version 0.
uint32_t parseTomb(const char* line, int len, int* wordLen) {
  int p = -1;
  for (int i = len - 1; i >= 0; i--)
    if (line[i] == '|') {
      p = i;
      break;
    }
  if (p < 0) {
    *wordLen = len;
    return 0;
  }
  *wordLen = p;
  return parseU32(line + p + 1, len - (p + 1));
}

bool tombLineWordEquals(const char* line, int len, const std::string& word) {
  int wl = 0;
  parseTomb(line, len, &wl);
  return static_cast<size_t>(wl) == word.size() && memcmp(line, word.c_str(), static_cast<size_t>(wl)) == 0;
}

// Single-number Lamport counter file IO (dictionary_flashcards.ver).
uint32_t readCounterFile(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("FCD", path, f)) return 0;
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
  if (!Storage.openFileForWrite("FCD", path, f)) return false;
  char buf[16];
  const int n = snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v));
  const bool ok = f.write(buf, static_cast<size_t>(n)) == static_cast<size_t>(n);
  f.close();
  return ok;
}

// Build the wire card payload "word|chapter|version|excerpt" (no box/dueDay)
// from a parsed deck line into buf. Returns the length, or -1 if it would not
// fit (defensive; a payload is always shorter than its source deck line).
int buildCardPayload(char* buf, int cap, const char* line, const Parsed& p) {
  char ver[12];
  const int vlen = snprintf(ver, sizeof(ver), "%lu", static_cast<unsigned long>(p.version));
  const int total = p.wordLen + 1 + p.chapterLen + 1 + vlen + 1 + p.excerptLen;
  if (total > cap) return -1;
  int n = 0;
  memcpy(buf + n, line, static_cast<size_t>(p.wordLen));
  n += p.wordLen;
  buf[n++] = '|';
  if (p.chapterLen > 0) memcpy(buf + n, p.chapter, static_cast<size_t>(p.chapterLen));
  n += p.chapterLen;
  buf[n++] = '|';
  memcpy(buf + n, ver, static_cast<size_t>(vlen));
  n += vlen;
  buf[n++] = '|';
  if (p.excerptLen > 0) memcpy(buf + n, p.excerpt, static_cast<size_t>(p.excerptLen));
  n += p.excerptLen;
  return n;
}

// Serialize accumulator shared by the tombstone / delta / rolling passes.
struct FcUploadCtx {
  uint8_t* out = nullptr;
  size_t used = 0;
  size_t cap = 0;
  uint32_t lastVer = 0;   // delta filter: emit cards/tombs with version > lastVer as deltas
  uint32_t maxVer = 0;    // highest version emitted (advances the watermark)
  int tombCount = 0;      // phase-1 NEW tombstones (version > lastVer)
  int histCount = 0;      // phase-2 delta cards (version > lastVer): genuinely new/changed
  int rollCount = 0;      // phase-4 rolling-slice cards (version <= lastVer): re-broadcast heal
  int tombRollCount = 0;  // phase-3 rolling-slice tombstones (version <= lastVer): re-broadcast heal
  bool truncated = false;
  int fileIdx = 0;               // running deck index for the card rolling passes
  uint32_t cursorStart = 0;      // card rolling resume index (deck file index)
  int nextCursor = 0;            // index after the last rolling card emitted
  int tombFileIdx = 0;           // running tomb index for the tomb rolling passes
  uint32_t tombCursorStart = 0;  // tomb rolling resume index (tomb file index)
  int nextTombCursor = 0;        // index after the last rolling tomb emitted
  int phase = 0;                 // rolling sub-pass: 1 = [cursorStart, N), 2 = [0, cursorStart)
};

// Append "tag<payload>\n" if it fits; set truncated + return false otherwise.
// "word|dictHash|version" for a 'D' line: the dictionary a card was saved from.
//
// A SEPARATE line type rather than an eighth field on 'H', because the card decoder in
// mergeBlob() finds exactly three pipes and then takes the excerpt as everything after the
// third. An older device reading an extended 'H' would therefore store "dictHash|excerpt" as
// the excerpt and re-broadcast that corruption through the rolling heal. Unknown line types,
// by contrast, are skipped by construction (`lineStart[0] != want`), so 'D' is invisible to
// every build that predates it.
int buildDictPayload(char* buf, int cap, const char* line, const Parsed& p) {
  char nums[24];
  const int nlen = snprintf(nums, sizeof(nums), "|%lu|%lu", static_cast<unsigned long>(p.dictHash),
                            static_cast<unsigned long>(p.version));
  if (p.wordLen + nlen > cap) return -1;
  memcpy(buf, line, static_cast<size_t>(p.wordLen));
  memcpy(buf + p.wordLen, nums, static_cast<size_t>(nlen));
  return p.wordLen + nlen;
}

bool fcEmit(FcUploadCtx* c, char tag, const char* payload, int plen) {
  const size_t need = 1 + static_cast<size_t>(plen) + 1;
  if (c->used + need > c->cap) {
    c->truncated = true;
    return false;
  }
  c->out[c->used++] = static_cast<uint8_t>(tag);
  memcpy(c->out + c->used, payload, static_cast<size_t>(plen));
  c->used += static_cast<size_t>(plen);
  c->out[c->used++] = static_cast<uint8_t>('\n');
  return true;
}

}  // namespace

// --- Lamport clock ---------------------------------------------------------

uint32_t FlashcardDeck::loadCounter(const std::string& cachePath) {
  const std::string verPath = verFilePath(cachePath);
  uint32_t cur = readCounterFile(verPath);
  if (cur == 0) {
    // .ver missing/zero: reconstruct as the max version across deck + tombstones
    // (covers upgrade from unversioned decks and a lost counter), then persist.
    uint32_t mx = 0;
    forEachLine(
        filePath(cachePath),
        [](void* ctx, const char* line, int len) {
          auto* m = static_cast<uint32_t*>(ctx);
          const uint32_t v = parseLine(line, len).version;
          if (v > *m) *m = v;
          return true;
        },
        &mx);
    forEachLine(
        tombFilePath(cachePath),
        [](void* ctx, const char* line, int len) {
          auto* m = static_cast<uint32_t*>(ctx);
          int wl = 0;
          const uint32_t v = parseTomb(line, len, &wl);
          if (v > *m) *m = v;
          return true;
        },
        &mx);
    cur = mx;
    if (cur > 0) writeCounterFile(verPath, cur);
  }
  return cur;
}

uint32_t FlashcardDeck::nextVersion(const std::string& cachePath) {
  const uint32_t v = loadCounter(cachePath) + 1;
  writeCounterFile(verFilePath(cachePath), v);
  return v;
}

void FlashcardDeck::observeVersion(const std::string& cachePath, uint32_t v) {
  if (v > loadCounter(cachePath)) writeCounterFile(verFilePath(cachePath), v);
}

// --- Per-word version lookups ----------------------------------------------

int FlashcardDeck::cardVersionOf(const std::string& cachePath, const std::string& word) {
  struct C {
    const std::string* word;
    int ver;  // -1 = absent
  } c{&word, -1};
  forEachLine(
      filePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<C*>(ctx);
        if (lineWordEquals(line, len, *c->word)) {
          const int v = static_cast<int>(parseLine(line, len).version);
          if (v > c->ver) c->ver = v;
        }
        return true;
      },
      &c);
  return c.ver;
}

uint32_t FlashcardDeck::tombstoneVersionOf(const std::string& cachePath, const std::string& word) {
  struct C {
    const std::string* word;
    uint32_t ver;
  } c{&word, 0};
  forEachLine(
      tombFilePath(cachePath),
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<C*>(ctx);
        if (tombLineWordEquals(line, len, *c->word)) {
          int wl = 0;
          const uint32_t v = parseTomb(line, len, &wl);
          if (v > c->ver) c->ver = v;
        }
        return true;
      },
      &c);
  return c.ver;
}

// --- Tombstone file (dictionary_flashcards.tomb) ---------------------------

void FlashcardDeck::setTombstone(const std::string& cachePath, const std::string& word, uint32_t version) {
  const std::string tomb = tombFilePath(cachePath);
  const std::string tmp = cachePath + "/dictionary_flashcards.tomb.tmp";
  HalFile out;
  if (!Storage.openFileForWrite("FCD", tmp, out)) {
    LOG_ERR("FCD", "Failed to open tomb temp: %s", tmp.c_str());
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
        if (!tombLineWordEquals(line, len, *c->word))
          c->ok = c->ok && writeRaw(*c->out, line, len) && c->out->write("\n", 1) == 1;
        return c->ok;
      },
      &wc);
  char tail[16];
  const int n = snprintf(tail, sizeof(tail), "|%lu\n", static_cast<unsigned long>(version));
  wc.ok = wc.ok && out.write(word.c_str(), word.size()) == word.size() && writeRaw(out, tail, n);
  out.close();
  if (!wc.ok) {
    LOG_ERR("FCD", "Tombstone write failed: %s", tmp.c_str());
    Storage.remove(tmp.c_str());
    return;
  }
  Storage.remove(tomb.c_str());
  Storage.rename(tmp.c_str(), tomb.c_str());
}

void FlashcardDeck::clearTombstone(const std::string& cachePath, const std::string& word) {
  const std::string tomb = tombFilePath(cachePath);
  // Scan first: skip the rewrite when there's no tombstone (the common case).
  struct P {
    const std::string* word;
    bool found;
  } pc{&word, false};
  forEachLine(
      tomb,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<P*>(ctx);
        if (tombLineWordEquals(line, len, *c->word)) {
          c->found = true;
          return false;
        }
        return true;
      },
      &pc);
  if (!pc.found) return;

  const std::string tmp = cachePath + "/dictionary_flashcards.tomb.tmp";
  HalFile out;
  if (!Storage.openFileForWrite("FCD", tmp, out)) return;
  struct C {
    const std::string* word;
    HalFile* out;
    bool ok;
  } wc{&word, &out, true};
  forEachLine(
      tomb,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<C*>(ctx);
        if (!tombLineWordEquals(line, len, *c->word))
          c->ok = c->ok && writeRaw(*c->out, line, len) && c->out->write("\n", 1) == 1;
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

// --- Merge primitives (no version bump; the wire version is authoritative) --

void FlashcardDeck::removeCardRow(const std::string& cachePath, const std::string& word) {
  CountCtx cc{&word, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
  if (!forEachLine(filePath(cachePath), countLine, &cc) || !cc.dupSeen) return;
  rewriteDeck(cachePath, const_cast<std::string*>(&word), [](void* ctx, HalFile& out, const char* line, int len) {
    const auto* w = static_cast<const std::string*>(ctx);
    if (lineWordEquals(line, len, *w)) return true;  // drop the matching row
    return writeRaw(out, line, len) && out.write("\n", 1) == 1;
  });
}

bool FlashcardDeck::appendRemoteCard(const std::string& cachePath, const std::string& word, const char* chapter,
                                     int chapterLen, const char* excerpt, int excerptLen, uint32_t version) {
  clearTombstone(cachePath, word);  // a newer add beats an old tombstone
  HalFile out;
  const std::string path = filePath(cachePath);
  if (!Storage.openFileForAppend("FCD", path.c_str(), out)) {
    LOG_ERR("FCD", "Failed to append remote card: %s", path.c_str());
    return false;
  }
  // Remote-received card: local lookup count starts at 1 (count is not synced), and dictHash
  // starts at 0 (unrecorded) -- the peer's 'D' line, if it sent one, sets it in the later pass.
  const bool ok =
      writeCard(out, word.c_str(), word.size(), 0, 0, chapter, chapterLen, version, 1, 0, excerpt, excerptLen);
  out.close();
  return ok;
}

// Apply a 'D' line: set the dictionary a card was saved from, leaving every other field alone.
// Separate from updateRemoteCard because the association travels on its own line and may arrive
// for a card this device already had (no content change) as well as one that just landed.
bool FlashcardDeck::setCardDict(const std::string& cachePath, const std::string& word, uint32_t dictHash) {
  // Scan before rewriting. rewriteDeck reads the whole deck, writes a temp copy and renames, so
  // it is far too expensive to run unconditionally here: the rolling heal re-broadcasts a 'D'
  // for every card in its slice on every sync, and nearly all of those carry a value this device
  // already has. Without this guard a sync costs one full deck rewrite per healed card.
  CountCtx cc{&word, 0, false, {}, 0, {}, 0, 0, 0, 0, 0};
  if (!forEachLine(filePath(cachePath), countLine, &cc) || !cc.dupSeen) return false;
  if (cc.savedDictHash == dictHash) return true;  // already correct — no I/O, no SD wear

  struct D {
    const std::string* word;
    uint32_t dictHash;
  } d{&word, dictHash};
  return rewriteDeck(cachePath, &d, [](void* ctx, HalFile& out, const char* line, int len) {
    auto* c = static_cast<D*>(ctx);
    const Parsed p = parseLine(line, len);
    if (static_cast<size_t>(p.wordLen) != c->word->size() || memcmp(line, c->word->c_str(), p.wordLen) != 0)
      return writeRaw(out, line, len) && out.write("\n", 1) == 1;  // copy verbatim
    // Only the dictionary changes. box/dueDay are this device's own schedule and are never
    // synced; version/count/chapter/excerpt are whatever the 'H' pass already settled.
    return writeCard(out, line, static_cast<size_t>(p.wordLen), p.box, p.dueDay, p.chapter, p.chapterLen, p.version,
                     p.count, c->dictHash, p.excerpt, p.excerptLen);
  });
}

bool FlashcardDeck::updateRemoteCard(const std::string& cachePath, const std::string& word, const char* chapter,
                                     int chapterLen, const char* excerpt, int excerptLen, uint32_t version) {
  struct U {
    const std::string* word;
    const char* chapter;
    int chapterLen;
    const char* excerpt;
    int excerptLen;
    uint32_t version;
  } u{&word, chapter, chapterLen, excerpt, excerptLen, version};
  return rewriteDeck(cachePath, &u, [](void* ctx, HalFile& out, const char* line, int len) {
    auto* c = static_cast<U*>(ctx);
    const Parsed p = parseLine(line, len);
    if (static_cast<size_t>(p.wordLen) != c->word->size() || memcmp(line, c->word->c_str(), p.wordLen) != 0)
      return writeRaw(out, line, len) && out.write("\n", 1) == 1;  // copy verbatim
    // Field-level: keep the local schedule (box/dueDay), local lookup count and local dictHash,
    // take the wire content (chapter/version/excerpt). dictHash is deliberately NOT taken from
    // the card line -- it does not travel there. Its own 'D' line carries it, applied in a later
    // pass, so a peer that never sends one leaves whatever this device already knew.
    return writeCard(out, line, static_cast<size_t>(p.wordLen), p.box, p.dueDay, c->chapter, c->chapterLen, c->version,
                     p.count, p.dictHash, c->excerpt, c->excerptLen);
  });
}

// --- Watermark (dictionary_flashcards.sync) --------------------------------

FlashcardDeck::SyncWatermark FlashcardDeck::loadWatermark(const std::string& cachePath) {
  SyncWatermark wm;
  HalFile f;
  if (!Storage.openFileForRead("FCD", syncFilePath(cachePath), f)) return wm;  // unset -> {0,0,0,0}
  // 4th field (tombCursorIndex) is optional: legacy 3-field files leave it 0.
  uint32_t* field[4] = {&wm.lastVer, &wm.cursorIndex, &wm.lastDeviceCount, &wm.tombCursorIndex};
  int fi = 0;
  char buf[16];
  int n = 0;
  bool inNum = false;
  while (f.available() && fi < 4) {
    const int b = f.read();
    if (b >= '0' && b <= '9') {
      if (n < 15) buf[n++] = static_cast<char>(b);
      inNum = true;
    } else if (inNum) {
      *field[fi++] = parseU32(buf, n);
      n = 0;
      inNum = false;
    }
  }
  if (inNum && fi < 4) *field[fi] = parseU32(buf, n);
  return wm;
}

void FlashcardDeck::storeWatermark(const std::string& cachePath, const SyncWatermark& wm) {
  HalFile f;
  if (!Storage.openFileForWrite("FCD", syncFilePath(cachePath), f)) {
    LOG_ERR("FCD", "Failed to write sync watermark: %s", syncFilePath(cachePath).c_str());
    // Serial is unreachable on a USB-locked X3; mirror to SD so a stuck watermark
    // (which keeps re-uploading the same "new:+N" cards) is diagnosable from the card.
    SdDebugLog::log("FCD", "WATERMARK write FAILED: %s", syncFilePath(cachePath).c_str());
    return;
  }
  char buf[56];
  const int n = snprintf(buf, sizeof(buf), "%lu %lu %lu %lu", static_cast<unsigned long>(wm.lastVer),
                         static_cast<unsigned long>(wm.cursorIndex), static_cast<unsigned long>(wm.lastDeviceCount),
                         static_cast<unsigned long>(wm.tombCursorIndex));
  f.write(buf, static_cast<size_t>(n));
  f.close();
}

// --- Adaptive slice cap ----------------------------------------------------

size_t FlashcardDeck::adaptiveSliceCap(uint32_t freeHeap, uint32_t deviceCount) {
  // Grow toward the ceiling only when heap is comfortable. Thresholds are
  // heuristic; the clamp below is the actual safety guarantee.
  size_t cap = FC_SLICE_FLOOR;
  if (freeHeap >= 72 * 1024)
    cap = FC_SLICE_CEIL;
  else if (freeHeap >= 56 * 1024)
    cap = (FC_SLICE_FLOOR + FC_SLICE_CEIL) / 2;

  // Clamp so deviceCount * (per-device reserve + cap) stays under half the 64 KB
  // GET response cap -- the aggregate every peer double-buffers. Reserve covers a
  // maxed "dh" blob (4 KB) plus counters that may ride the same GET.
  constexpr size_t kGetHalfCap = 32 * 1024;
  constexpr size_t kPerDeviceReserve = 4 * 1024 + 256;
  const uint32_t devs = deviceCount > 0 ? deviceCount : 1;
  const size_t budget = kGetHalfCap / devs;
  const size_t maxCap = budget > kPerDeviceReserve ? budget - kPerDeviceReserve : 0;
  if (cap > maxCap) cap = maxCap;
  // The floor is always safe (8 * (2048 + 4352) = 51200 < 64 KB), so never go
  // below it even if the half-cap clamp would -- a sub-floor blob stalls backfill.
  if (cap < FC_SLICE_FLOOR) cap = FC_SLICE_FLOOR;
  return cap;
}

// --- Serialize (tombstones + delta cards + rolling slice) ------------------

size_t FlashcardDeck::serializeForUpload(const std::string& cachePath, uint8_t* out, size_t cap, BlobStats* outStats) {
  if (outStats) *outStats = BlobStats{};
  if (!out || cap == 0) return 0;

  const SyncWatermark wm = loadWatermark(cachePath);
  const std::string deckPath = filePath(cachePath);
  // SD-only (USB-locked X3): logs what the watermark READ BACK at the start of this
  // sync. Compare against the previous sync's "commitUpload: lastVer ...->B": if this
  // read shows a lastVer LOWER than the last committed B, the watermark write isn't
  // persisting (the cards keep looking "new" -> perpetual "new:+N").
  SdDebugLog::log("FCD", "serializeForUpload: read lastVer=%lu cursor=%lu tombCursor=%lu",
                  static_cast<unsigned long>(wm.lastVer), static_cast<unsigned long>(wm.cursorIndex),
                  static_cast<unsigned long>(wm.tombCursorIndex));

  FcUploadCtx c;
  c.out = out;
  c.cap = cap;
  c.lastVer = wm.lastVer;
  c.cursorStart = wm.cursorIndex;
  c.nextCursor = static_cast<int>(wm.cursorIndex);
  c.tombCursorStart = wm.tombCursorIndex;
  c.nextTombCursor = static_cast<int>(wm.tombCursorIndex);

  const std::string tombPath = tombFilePath(cachePath);

  // 1) NEW tombstones (version > watermark) -- low-latency delete push, mirroring the
  //    delta-card pass. Older tombs are NOT re-sent here (they would re-broadcast the
  //    entire pile every sync); they heal via the rolling pass below. Emitted first so
  //    a delete still beats a concurrent re-add on the receiver (mergeBlob's T-before-H).
  forEachLine(
      tombPath,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<FcUploadCtx*>(ctx);
        int wl = 0;
        const uint32_t v = parseTomb(line, len, &wl);
        if (v <= c->lastVer) return true;              // old delete -> handled by the rolling pass
        if (!fcEmit(c, 'T', line, len)) return false;  // budget full -> stop
        c->tombCount++;
        if (v > c->maxVer) c->maxVer = v;
        return true;
      },
      &c);

  // 2) Changed cards (version > watermark) -- low-latency new-enrollment push.
  forEachLine(
      deckPath,
      [](void* ctx, const char* line, int len) {
        auto* c = static_cast<FcUploadCtx*>(ctx);
        const Parsed p = parseLine(line, len);
        if (p.version <= c->lastVer) return true;  // not new since last upload
        char buf[512];
        const int n = buildCardPayload(buf, sizeof(buf), line, p);
        if (n < 0) return true;  // pathological oversize; skip
        if (!fcEmit(c, 'H', buf, n)) return false;
        c->histCount++;
        if (p.version > c->maxVer) c->maxVer = p.version;
        // The dictionary association rides its own line, emitted only when there is one. If the
        // budget runs out between the two, the card still lands on the peer with correct front
        // content and no association; the rolling heal below re-sends it on a later sync.
        if (p.dictHash != 0) {
          const int dn = buildDictPayload(buf, sizeof(buf), line, p);
          if (dn >= 0 && !fcEmit(c, 'D', buf, dn)) return false;
        }
        return true;
      },
      &c);

  // 3) Rolling tomb slice: older tombstones (version <= watermark) from the tomb cursor,
  //    wrapping, to heal a fresh device's delete set over successive syncs. Bounds the
  //    per-sync tomb cost to the budget instead of re-sending all tombs every time.
  //    Runs BEFORE the card heal so a large deck can't starve delete propagation (the
  //    tomb set is small, so it completes a full pass quickly). Two sub-passes over the
  //    tomb file: [tombCursorStart, M) then [0, tombCursorStart).
  for (int phase = 1; phase <= 2; phase++) {
    c.tombFileIdx = 0;
    c.phase = phase;
    forEachLine(
        tombPath,
        [](void* ctx, const char* line, int len) {
          auto* c = static_cast<FcUploadCtx*>(ctx);
          const int idx = c->tombFileIdx++;
          if (c->phase == 1 && idx < static_cast<int>(c->tombCursorStart)) return true;    // before the window
          if (c->phase == 2 && idx >= static_cast<int>(c->tombCursorStart)) return false;  // past the wrap region
          int wl = 0;
          const uint32_t v = parseTomb(line, len, &wl);
          if (v > c->lastVer) return true;               // a new tomb already covered it (phase 1)
          if (!fcEmit(c, 'T', line, len)) return false;  // budget full -> stop rolling
          c->tombRollCount++;                            // re-broadcast heal, not a new delete
          c->nextTombCursor = idx + 1;                   // resume after this tomb next sync
          return true;
        },
        &c);
    if (c.truncated) break;  // no budget left for the wrap pass
  }

  // 4) Rolling card slice: older cards (version <= watermark) from the card cursor,
  //    wrapping, to heal a fresh device over successive syncs. Two sub-passes over the
  //    deck file: [cursorStart, N) then [0, cursorStart).
  for (int phase = 1; phase <= 2; phase++) {
    c.fileIdx = 0;
    c.phase = phase;
    forEachLine(
        deckPath,
        [](void* ctx, const char* line, int len) {
          auto* c = static_cast<FcUploadCtx*>(ctx);
          const int idx = c->fileIdx++;
          if (c->phase == 1 && idx < static_cast<int>(c->cursorStart)) return true;    // before the window
          if (c->phase == 2 && idx >= static_cast<int>(c->cursorStart)) return false;  // past the wrap region
          const Parsed p = parseLine(line, len);
          if (p.version > c->lastVer) return true;  // a delta card already covered it
          char buf[512];
          const int n = buildCardPayload(buf, sizeof(buf), line, p);
          if (n < 0) return true;
          if (!fcEmit(c, 'H', buf, n)) return false;  // budget full -> stop rolling
          c->rollCount++;                             // re-broadcast heal, not a new delta
          if (p.dictHash != 0) {
            const int dn = buildDictPayload(buf, sizeof(buf), line, p);
            if (dn >= 0 && !fcEmit(c, 'D', buf, dn)) return false;
          }
          c->nextCursor = idx + 1;  // resume after this card next sync
          return true;
        },
        &c);
    if (c.truncated) break;  // no budget left for the wrap pass
  }

  if (outStats) {
    outStats->histCount = c.histCount;
    outStats->rollCount = c.rollCount;
    outStats->tombCount = c.tombCount;
    outStats->tombRollCount = c.tombRollCount;
    outStats->maxVer = c.maxVer;
    outStats->nextCursor = static_cast<uint32_t>(c.nextCursor);
    outStats->nextTombCursor = static_cast<uint32_t>(c.nextTombCursor);
    outStats->truncated = c.truncated;
  }
  return c.used;
}

void FlashcardDeck::commitUpload(const std::string& cachePath, const BlobStats& uploaded, uint32_t deviceCount) {
  SyncWatermark wm = loadWatermark(cachePath);
  const uint32_t prevVer = wm.lastVer;
  if (uploaded.maxVer > wm.lastVer) wm.lastVer = uploaded.maxVer;  // never regress
  wm.cursorIndex = uploaded.nextCursor;
  wm.tombCursorIndex = uploaded.nextTombCursor;
  wm.lastDeviceCount = deviceCount;
  storeWatermark(cachePath, wm);
  // SD-only (USB-locked X3 has no serial): records that the upload watermark moved.
  // If "new:+N" persists across syncs while lastVer stops advancing here, the cards
  // keep re-uploading because this commit was skipped (failed PUT) or didn't progress.
  SdDebugLog::log("FCD",
                  "commitUpload: lastVer %lu->%lu cursor->%lu tombCursor->%lu hist=%d tomb=%d roll=%d tombRoll=%d",
                  static_cast<unsigned long>(prevVer), static_cast<unsigned long>(wm.lastVer),
                  static_cast<unsigned long>(wm.cursorIndex), static_cast<unsigned long>(wm.tombCursorIndex),
                  uploaded.histCount, uploaded.tombCount, uploaded.rollCount, uploaded.tombRollCount);
}

// --- Merge -----------------------------------------------------------------

namespace {
// Liveness pump — see FlashcardDeck::setMergeProgressHook.
void (*s_mergePumpFn)(void* ctx, size_t done, size_t total) = nullptr;
void* s_mergePumpCtx = nullptr;
}  // namespace

void FlashcardDeck::setMergeProgressHook(void (*fn)(void* ctx, size_t done, size_t total), void* ctx) {
  s_mergePumpFn = fn;
  s_mergePumpCtx = ctx;
}

int FlashcardDeck::mergeBlob(const std::string& cachePath, const uint8_t* blob, size_t len, int* outDeleted) {
  if (outDeleted) *outDeleted = 0;
  if (!blob || len == 0) return 0;

  // RAM-only pre-pass: raise the Lamport clock ONCE, before any edit lands, instead of
  // once per blob line. observeVersion() re-reads the counter file on every call, so the
  // per-line form cost a file open per line on its own. The admission tests below mirror
  // the merge loop exactly, so the clock is raised past the same versions and no others.
  {
    uint32_t maxVer = 0;
    size_t i = 0;
    while (i < len) {
      size_t j = i;
      while (j < len && blob[j] != '\n') j++;
      const char* lineStart = reinterpret_cast<const char*>(blob + i);
      const int lineLen = static_cast<int>(j - i);
      i = j + 1;
      if (lineLen < 2) continue;
      const char* payload = lineStart + 1;
      const int plen = lineLen - 1;
      if (lineStart[0] == 'T') {
        int wl = 0;
        const uint32_t ver = parseTomb(payload, plen, &wl);
        if (wl > 0 && ver > maxVer) maxVer = ver;
      } else if (lineStart[0] == 'H') {
        int p0 = -1, p1 = -1, p2 = -1;
        for (int k = 0; k < plen; k++)
          if (payload[k] == '|') {
            if (p0 < 0)
              p0 = k;
            else if (p1 < 0)
              p1 = k;
            else {
              p2 = k;
              break;
            }
          }
        if (p0 < 0 || p1 < 0 || p2 < 0) continue;
        const uint32_t ver = parseU32(payload + p1 + 1, p2 - p1 - 1);
        if (ver > maxVer) maxVer = ver;
      } else if (lineStart[0] == 'D') {
        // "word|dictHash|version". Counted here for the same reason 'H' is: the clock must be
        // raised past every version the blob carries, and a 'D' can be the only line for a card
        // whose content the peer already had.
        int p0 = -1, p1 = -1;
        for (int k = 0; k < plen; k++)
          if (payload[k] == '|') {
            if (p0 < 0)
              p0 = k;
            else {
              p1 = k;
              break;
            }
          }
        if (p0 < 0 || p1 < 0) continue;
        const uint32_t ver = parseU32(payload + p1 + 1, plen - (p1 + 1));
        if (ver > maxVer) maxVer = ver;
      }
    }
    observeVersion(cachePath, maxVer);  // materializes the clock too (loadCounter inside)
  }

  int added = 0;
  int deleted = 0;

  // Three passes: tombstones first, then cards, so a re-enroll (higher version) correctly beats
  // a delete for the same word -- then dictionary associations, which must run LAST because a
  // 'D' line is only applicable to a card that exists, and its card may be arriving in the very
  // same blob's 'H' pass.
  for (int pass = 0; pass < 3; pass++) {
    const char want = (pass == 0) ? 'T' : (pass == 1) ? 'H' : 'D';
    size_t i = 0;
    while (i < len) {
      size_t j = i;
      while (j < len && blob[j] != '\n') j++;
      const char* lineStart = reinterpret_cast<const char*>(blob + i);
      const int lineLen = static_cast<int>(j - i);
      i = j + 1;
      if (s_mergePumpFn) s_mergePumpFn(s_mergePumpCtx, static_cast<size_t>(pass) * len + i, len * 2);
      if (lineLen < 2 || lineStart[0] != want) continue;
      const char* payload = lineStart + 1;
      const int plen = lineLen - 1;

      if (want == 'T') {
        int wl = 0;
        const uint32_t ver = parseTomb(payload, plen, &wl);
        if (wl <= 0) continue;
        // Brace-init, NOT word(...): Arduino.h defines a function-like macro word(...)
        // plus a `word` typedef, so `std::string word(payload, n)` expands the macro and
        // `word` never becomes a variable. Braces avoid the macro (no `word(`).
        std::string word{payload, static_cast<size_t>(wl)};
        const int cardVer = cardVersionOf(cachePath, word);
        const uint32_t tombVer = tombstoneVersionOf(cachePath, word);
        const uint32_t localVer = std::max(cardVer >= 0 ? static_cast<uint32_t>(cardVer) : 0u, tombVer);
        if (ver <= localVer) continue;  // our copy is newer-or-equal
        removeCardRow(cachePath, word);
        setTombstone(cachePath, word, ver);
        deleted++;
      } else if (want == 'H') {
        // payload = "word|chapter|version|excerpt" (chapter has no '|'; excerpt
        // is the remainder and may).
        int p0 = -1, p1 = -1, p2 = -1;
        for (int k = 0; k < plen; k++)
          if (payload[k] == '|') {
            if (p0 < 0)
              p0 = k;
            else if (p1 < 0)
              p1 = k;
            else {
              p2 = k;
              break;
            }
          }
        if (p0 < 0 || p1 < 0 || p2 < 0) continue;
        // Brace-init: avoids the Arduino word(...) macro — see the 'T' branch above.
        std::string word{payload, static_cast<size_t>(p0)};
        const char* chapter = payload + p0 + 1;
        const int chapterLen = p1 - p0 - 1;
        const uint32_t ver = parseU32(payload + p1 + 1, p2 - p1 - 1);
        const char* excerpt = payload + p2 + 1;
        const int excerptLen = plen - (p2 + 1);
        const int cardVer = cardVersionOf(cachePath, word);
        if (cardVer >= 0) {
          if (ver <= static_cast<uint32_t>(cardVer)) continue;  // have it, not newer
          updateRemoteCard(cachePath, word, chapter, chapterLen, excerpt, excerptLen, ver);
        } else {
          const uint32_t tombVer = tombstoneVersionOf(cachePath, word);
          if (tombVer > 0 && ver <= tombVer) continue;  // a newer/equal delete still wins
          appendRemoteCard(cachePath, word, chapter, chapterLen, excerpt, excerptLen, ver);
          added++;
        }
      } else {  // want == 'D': "word|dictHash|version"
        int p0 = -1, p1 = -1;
        for (int k = 0; k < plen; k++)
          if (payload[k] == '|') {
            if (p0 < 0)
              p0 = k;
            else {
              p1 = k;
              break;
            }
          }
        if (p0 < 0 || p1 < 0) continue;
        // Brace-init: avoids the Arduino word(...) macro — see the 'T' branch above.
        std::string word{payload, static_cast<size_t>(p0)};
        const uint32_t dictHash = parseU32(payload + p0 + 1, p1 - (p0 + 1));
        const uint32_t ver = parseU32(payload + p1 + 1, plen - (p1 + 1));
        if (dictHash == 0) continue;  // nothing to record
        const int cardVer = cardVersionOf(cachePath, word);
        if (cardVer < 0) continue;  // no such card here (deleted, or never arrived) — drop it
        // >= not >: the 'D' carries its card's OWN version, so after the 'H' pass above has
        // written that version locally the two are equal and the association must still apply.
        // A strictly newer local version means this device re-enrolled the word since, and its
        // own association wins.
        if (ver < static_cast<uint32_t>(cardVer)) continue;
        setCardDict(cachePath, word, dictHash);
      }
    }
  }
  if (outDeleted) *outDeleted = deleted;
  return added;
}
