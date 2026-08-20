#include "Dictionary.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

// Static member definitions
char Dictionary::wordBuf[256] = "";
uint8_t Dictionary::IdxScanner::buf_[Dictionary::IdxScanner::BUF_SIZE] = {};
char Dictionary::sessionPath[128] = "";
char Dictionary::preFallbackPath[128] = "";
bool Dictionary::sessionPathIsFallback = false;

namespace {
constexpr char DICT_BIN[] = "dictionary.bin";
constexpr char GLOBAL_DICT_DIR[] = "/.crosspoint";

// Word characters for edge-trimming: ASCII alphanumerics plus any UTF-8 lead or
// continuation byte, so accented, Cyrillic and CJK words keep their edges. Plain
// std::isalnum() rejects every byte >= 0x80, which trimmed "café" to "caf" and
// reduced wholly non-ASCII words ("漢字", "Привет") to "" — an empty lookup.
bool isWordByte(unsigned char c) { return c >= 0x80 || std::isalnum(c) != 0; }

// True when b[i] starts a General Punctuation codepoint (U+2000-U+206F, encoded
// E2 80 xx / E2 81 xx): curly quotes, en/em dashes, ellipsis. isWordByte keeps
// these because they are >= 0x80, so they must be trimmed explicitly or EPUB
// text like garage.” never matches a headword. Callers guarantee 3 bytes are
// readable from i. (#2877)
bool isGeneralPunctuationAt(const unsigned char* b, size_t i) {
  return b[i] == 0xE2 && (b[i + 1] == 0x80 || b[i + 1] == 0x81);
}

// True when b[i] starts a CJK punctuation codepoint: the CJK Symbols and Punctuation
// block (。、「」《》【】, U+3000-U+303F, encoded E3 80 xx) and the Fullwidth Forms
// (，！？：（）, U+FF01-U+FF65, encoded EF BC/BD xx), plus the vertical punctuation
// forms. Like General Punctuation above, isWordByte keeps all of these because they
// are >= 0x80, so they must be trimmed explicitly or a Chinese word touching
// punctuation never matches a headword.
//
// They arrive glued to the word rather than as their own token: layout forbids a line
// break before closing punctuation and after opening punctuation
// (ParsedText.cpp:150-155), so 國。 and 「中 are each a single token by design.
//
// U+3005-U+3007 and U+303B fall inside that first block but are content, not
// punctuation: 々 repeats the preceding character (人々), 〆 is an abbreviation mark and
// 〇 is the ideographic zero (二〇二五). Trimming them would reduce 人々。 to 人.
// utf8IsCjkPunctuation does not draw that distinction — it is also the selectability
// filter at DictionaryWordSelectActivity.cpp:70 — so the carve-out lives here, where
// the question is only what to strip off a lookup key.
//
// Every codepoint in these ranges is 3 bytes in UTF-8, so this slots into the existing
// 3-byte edge handling. Callers guarantee 3 bytes are readable from i; the encoding is
// still validated so a truncated tail is never decoded as punctuation.
bool isCjkPunctuationAt(const unsigned char* b, size_t i) {
  if (b[i] < 0xE0 || b[i] > 0xEF) return false;
  if ((b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80) return false;
  const uint32_t cp = (static_cast<uint32_t>(b[i] & 0x0F) << 12) | (static_cast<uint32_t>(b[i + 1] & 0x3F) << 6) |
                      static_cast<uint32_t>(b[i + 2] & 0x3F);
  if (cp >= 0x3005 && cp <= 0x3007) return false;  // 々 〆 〇 — content, not punctuation
  if (cp == 0x303B) return false;                  // 〻 vertical iteration mark
  return utf8IsCjkPunctuation(cp);
}

// Either class of 3-byte punctuation that isWordByte would otherwise keep.
bool isTrimmable3ByteAt(const unsigned char* b, size_t i) {
  return isGeneralPunctuationAt(b, i) || isCjkPunctuationAt(b, i);
}
}  // namespace

// OFT file constants (StarDict Cache format, verified against real files).
// Header: 30-byte text + 8-byte fixed magic = 38 bytes total.
// Each entry: LE uint32 = byte offset of word (K+1)*32 in the source file.
static constexpr uint32_t OFT_HEADER_SIZE = 38;
static constexpr uint32_t OFT_STRIDE = 32;  // words per page

// .idx.oft.cspt file constants (CrossPoint optimized index).
// Header: 12 bytes = magic(4) + version(1) + prefixLen(1) + stride(2) + entryCount(4).
// Each entry: prefixLen bytes (null-padded headword) + 4-byte LE idx offset = 20 bytes.
static constexpr uint8_t CSPT_MAGIC[4] = {'C', 'S', 'P', 'T'};
static constexpr uint8_t CSPT_VERSION = 1;
static constexpr uint8_t CSPT_PREFIX_LEN = 16;
static constexpr uint16_t CSPT_STRIDE = 16;
static constexpr uint32_t CSPT_HEADER_SIZE = 12;
static constexpr uint32_t CSPT_ENTRY_SIZE = CSPT_PREFIX_LEN + 4;  // 20 bytes

// ---------------------------------------------------------------------------
// Path management
// ---------------------------------------------------------------------------

std::string Dictionary::readDictPath(const char* cachePath) {
  char binPath[128];

  // Try per-book dictionary.bin first when cachePath is provided.
  if (cachePath && cachePath[0] != '\0') {
    snprintf(binPath, sizeof(binPath), "%s/%s", cachePath, DICT_BIN);
    HalFile f;
    if (Storage.openFileForRead("DICT", binPath, f)) {
      const int sz = static_cast<int>(f.fileSize());
      if (sz > 0) {
        std::string result(sz, '\0');
        const int n = f.read(&result[0], sz);
        f.close();
        if (n > 0) {
          result.resize(static_cast<size_t>(n));
          return result;
        }
      } else {
        f.close();
      }
    }
    // Per-book file absent or empty ("Use Global") — fall through to global.
  }

  // Read global dictionary.bin.
  snprintf(binPath, sizeof(binPath), "%s/%s", GLOBAL_DICT_DIR, DICT_BIN);
  HalFile f;
  if (!Storage.openFileForRead("DICT", binPath, f)) return "";
  const int sz = static_cast<int>(f.fileSize());
  if (sz <= 0) {
    f.close();
    return "";
  }
  std::string result(sz, '\0');
  const int n = f.read(&result[0], sz);
  f.close();
  if (n <= 0) return "";
  result.resize(static_cast<size_t>(n));
  return result;
}

void Dictionary::saveGlobalDictPath(const char* folderPath) {
  char binPath[128];
  snprintf(binPath, sizeof(binPath), "%s/%s", GLOBAL_DICT_DIR, DICT_BIN);
  HalFile f;
  if (!Storage.openFileForWrite("DICT", binPath, f)) {
    LOG_ERR("DICT", "Could not write global dictionary path");
    return;
  }
  if (folderPath && folderPath[0] != '\0') {
    f.write(reinterpret_cast<const uint8_t*>(folderPath), strlen(folderPath));
  }
  f.close();
}

void Dictionary::setSessionDictPath(const char* folderPath) {
  // An explicit choice outranks any fallback promotion and drops its pending revert, so a
  // later takeFallbackPromotion() cannot undo what the user just chose.
  sessionPathIsFallback = false;
  preFallbackPath[0] = '\0';
  if (!folderPath || folderPath[0] == '\0') {
    sessionPath[0] = '\0';
    return;
  }
  snprintf(sessionPath, sizeof(sessionPath), "%s", folderPath);
}

void Dictionary::promoteFallbackDictPath(const char* folderPath) {
  if (!folderPath || folderPath[0] == '\0') return;
  // Only the first promotion records the revert target: a second one in a row would
  // otherwise overwrite it with the first promotion's own path.
  if (!sessionPathIsFallback) snprintf(preFallbackPath, sizeof(preFallbackPath), "%s", sessionPath);
  snprintf(sessionPath, sizeof(sessionPath), "%s", folderPath);
  sessionPathIsFallback = true;
}

bool Dictionary::sessionPathIsFallbackPromotion() { return sessionPathIsFallback; }

std::string Dictionary::takeFallbackPromotion() {
  if (!sessionPathIsFallback) return std::string();
  std::string promoted = sessionPath;
  snprintf(sessionPath, sizeof(sessionPath), "%s", preFallbackPath);  // "" = no override
  preFallbackPath[0] = '\0';
  sessionPathIsFallback = false;
  return promoted;
}

std::string Dictionary::activeDictPath(const char* cachePath) {
  if (sessionPath[0] != '\0') return sessionPath;
  return readDictPath(cachePath);
}

// ---------------------------------------------------------------------------
// Validity checks
// ---------------------------------------------------------------------------

bool Dictionary::exists(const char* cachePath) {
  std::string folderPath = activeDictPath(cachePath);
  if (folderPath.empty()) return false;
  DictPaths dp(folderPath);
  if (!Storage.exists(dp.idx().c_str())) return false;
  return Storage.exists(dp.dict().c_str());
}

bool Dictionary::hasAltForms(const char* cachePath) {
  std::string folderPath = activeDictPath(cachePath);
  if (folderPath.empty()) return false;
  return Storage.exists(DictPaths(folderPath).syn().c_str());
}

bool Dictionary::isValidDictionary() {
  std::string folderPath = readDictPath(nullptr);
  if (folderPath.empty()) return false;
  DictPaths dp(folderPath);
  const bool idxExists = Storage.exists(dp.idx().c_str());
  const bool valid = idxExists && Storage.exists(dp.dict().c_str());
  if (!valid) {
    LOG_DBG("DICT", "Stored dictionary path no longer valid, resetting");
    saveGlobalDictPath("");
  }
  return valid;
}

// ---------------------------------------------------------------------------
// .ifo parsing
// ---------------------------------------------------------------------------

bool Dictionary::readInfoInto(const char* folderPath, DictInfo& info) {
  info = DictInfo{};  // callers may reuse one scratch object across dictionaries

  if (folderPath == nullptr || folderPath[0] == '\0') return false;

  std::string folder(folderPath);
  std::string ifoPath = DictPaths(folder).ifo();

  HalFile file;
  if (!Storage.openFileForRead("DICT", ifoPath.c_str(), file)) return false;

  // Validate header line byte by byte — no line buffer needed.
  static constexpr const char HEADER[] = "StarDict's dict ifo file";
  for (size_t i = 0; i < sizeof(HEADER) - 1; i++) {
    int b = file.read();
    if (b < 0 || static_cast<char>(b) != HEADER[i]) {
      LOG_ERR("DICT", "Invalid .ifo header in %s", folderPath);
      file.close();
      return false;
    }
  }
  // Skip remainder of header line.
  {
    int b;
    while ((b = file.read()) >= 0 && b != '\n') {
    }
  }

  // Serial key=value parse. State fits in ~50 bytes vs the old 512-byte slurp buffer.
  char keyBuf[24];  // longest key: "sametypesequence" = 16 chars
  int keyLen = 0;
  bool readingVal = false;
  char* valDst = nullptr;
  size_t valCap = 0;
  size_t valWritten = 0;
  bool isNumField = false;
  uint32_t* valNum = nullptr;
  uint32_t numAccum = 0;

  while (file.available()) {
    int b = file.read();
    if (b < 0) break;
    const char c = static_cast<char>(b);

    if (c == '\r') continue;

    if (!readingVal) {
      if (c == '\n') {
        keyLen = 0;
      } else if (c == '=') {
        keyBuf[keyLen] = '\0';
        keyLen = 0;
        valDst = nullptr;
        valCap = 0;
        valWritten = 0;
        isNumField = false;
        valNum = nullptr;
        numAccum = 0;
        if (strcmp(keyBuf, "bookname") == 0) {
          valDst = info.bookname;
          valCap = sizeof(info.bookname) - 1;
        } else if (strcmp(keyBuf, "sametypesequence") == 0) {
          valDst = info.sametypesequence;
          valCap = sizeof(info.sametypesequence) - 1;
        } else if (strcmp(keyBuf, "website") == 0) {
          valDst = info.website;
          valCap = sizeof(info.website) - 1;
        } else if (strcmp(keyBuf, "date") == 0) {
          valDst = info.date;
          valCap = sizeof(info.date) - 1;
        } else if (strcmp(keyBuf, "description") == 0) {
          valDst = info.description;
          valCap = sizeof(info.description) - 1;
        } else if (strcmp(keyBuf, "lang") == 0) {
          valDst = info.lang;
          valCap = sizeof(info.lang) - 1;
        } else if (strcmp(keyBuf, "wordcount") == 0) {
          isNumField = true;
          valNum = &info.wordcount;
        } else if (strcmp(keyBuf, "idxfilesize") == 0) {
          isNumField = true;
          valNum = &info.idxfilesize;
        } else if (strcmp(keyBuf, "synwordcount") == 0) {
          isNumField = true;
          valNum = &info.altFormCount;
          info.hasAltForms = true;
        }
        readingVal = true;
      } else if (keyLen < static_cast<int>(sizeof(keyBuf) - 1)) {
        keyBuf[keyLen++] = c;
      }
    } else {
      if (c == '\n') {
        if (valDst) valDst[valWritten] = '\0';
        if (isNumField && valNum) *valNum = numAccum;
        readingVal = false;
        keyLen = 0;
      } else if (isNumField) {
        if (c >= '0' && c <= '9') numAccum = numAccum * 10 + static_cast<uint32_t>(c - '0');
      } else if (valDst && valWritten < valCap) {
        valDst[valWritten++] = c;
      }
    }
  }

  file.close();

  DictPaths dp(folder);
  const bool dictExists = Storage.exists(dp.dict().c_str());
  info.isCompressed = !dictExists && Storage.exists(dp.dictDz().c_str());

  info.valid = true;
  return true;
}

DictInfo Dictionary::readInfo(const char* folderPath) {
  DictInfo info;
  readInfoInto(folderPath, info);
  return info;
}

// ---------------------------------------------------------------------------
// Word cleaning
// ---------------------------------------------------------------------------

std::string Dictionary::cleanWord(const std::string& word) {
  if (word.empty()) return "";

  const auto* b = reinterpret_cast<const unsigned char*>(word.data());
  size_t start = 0;
  size_t end = word.size();

  // Trim non-word bytes from both edges, treating a General Punctuation or CJK
  // punctuation codepoint as a single 3-byte unit rather than three word bytes.
  while (start < end) {
    if (!isWordByte(b[start])) {
      start++;
    } else if (end - start >= 3 && isTrimmable3ByteAt(b, start)) {
      start += 3;
    } else {
      break;
    }
  }
  while (end > start) {
    if (!isWordByte(b[end - 1])) {
      end--;
    } else if (end - start >= 3 && isTrimmable3ByteAt(b, end - 3)) {
      end -= 3;
    } else {
      break;
    }
  }

  if (start >= end) return "";

  std::string result = word.substr(start, end - start);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
  return result;
}

// ---------------------------------------------------------------------------
// Low-level file reading helpers
// ---------------------------------------------------------------------------

int Dictionary::readWordInto(HalFile& file, char* buf, size_t bufSize) {
  size_t i = 0;
  while (i < bufSize - 1) {
    int ch = file.read();
    if (ch < 0) return -1;  // EOF or I/O error
    if (ch == 0) {
      buf[i] = '\0';
      return static_cast<int>(i);
    }
    buf[i++] = static_cast<char>(ch);
  }
  // Word too long for buffer — consume remaining bytes to stay in sync
  buf[bufSize - 1] = '\0';
  int ch;
  do {
    ch = file.read();
  } while (ch > 0);
  return static_cast<int>(bufSize - 1);
}

bool Dictionary::IdxScanner::refill() {
  if (eof_) return false;
  const int got = file_.read(buf_, BUF_SIZE);
  if (got <= 0) {
    eof_ = true;
    avail_ = 0;
    cursor_ = 0;
    return false;
  }
  avail_ = static_cast<size_t>(got);
  cursor_ = 0;
  // A short read is NOT treated as end-of-file: only a read that returns nothing latches eof_.
  // Costs at most one extra read per scan and avoids assuming the underlying reader short-reads
  // solely at EOF — an assumption that, if wrong, would silently truncate a scan into a miss.
  return true;
}

int Dictionary::IdxScanner::readByte() {
  if (cursor_ >= avail_ && !refill()) return -1;
  pos_++;
  return buf_[cursor_++];
}

bool Dictionary::IdxScanner::readBytes(void* dst, size_t count) {
  auto* out = static_cast<uint8_t*>(dst);
  while (count > 0) {
    if (cursor_ >= avail_ && !refill()) return false;
    const size_t chunk = std::min(count, avail_ - cursor_);
    memcpy(out, buf_ + cursor_, chunk);
    cursor_ += chunk;
    pos_ += static_cast<uint32_t>(chunk);
    out += chunk;
    count -= chunk;
  }
  return true;
}

void Dictionary::IdxScanner::seek(uint32_t offset) {
  file_.seekSet(offset);
  pos_ = offset;
  avail_ = 0;
  cursor_ = 0;
  // A previous short read may have latched eof_ while the new target is well inside the file.
  eof_ = false;
}

int Dictionary::readWordInto(IdxScanner& scanner, char* buf, size_t bufSize) {
  size_t i = 0;
  while (i < bufSize - 1) {
    const int ch = scanner.readByte();
    if (ch < 0) return -1;  // EOF or I/O error
    if (ch == 0) {
      buf[i] = '\0';
      return static_cast<int>(i);
    }
    buf[i++] = static_cast<char>(ch);
  }
  // Word too long for buffer — consume remaining bytes to stay in sync
  buf[bufSize - 1] = '\0';
  int ch;
  do {
    ch = scanner.readByte();
  } while (ch > 0);
  return static_cast<int>(bufSize - 1);
}

// ---------------------------------------------------------------------------
// OFT binary search helper
// ---------------------------------------------------------------------------

// Case-insensitive strcmp for ASCII — used in findPageBounds() because StarDict
// dictionaries (including wiktionary-derived ones) are sorted case-insensitively.
// Using plain strcmp would cause the binary search to land on the wrong page for
// any word whose alphabetic neighbourhood contains mixed-case page boundaries.
static int cistrcmp(const char* a, const char* b) {
  while (*a && *b) {
    int diff = std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
    if (diff != 0) return diff;
    a++;
    b++;
  }
  return std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
}

// CLEANUP: on Auto-only commit, delete only this line (readCsptEntryCount below stays)
uint32_t Dictionary::readCsptEntryCount(const char* cachePath) {
  std::string folderPath = activeDictPath(cachePath);
  if (folderPath.empty()) return 0;
  HalFile cspt;
  if (!Storage.openFileForRead("DICT", DictPaths(folderPath).idxOftCspt().c_str(), cspt)) return 0;
  uint8_t hdr[CSPT_HEADER_SIZE];
  cspt.seekSet(0);
  const bool read_ok = (cspt.read(hdr, CSPT_HEADER_SIZE) == static_cast<int>(CSPT_HEADER_SIZE));
  cspt.close();
  if (!read_ok) return 0;
  if (memcmp(hdr, CSPT_MAGIC, 4) != 0 || hdr[4] != CSPT_VERSION) return 0;
  uint32_t entryCount;
  memcpy(&entryCount, hdr + 8, 4);  // LE, matches binarySearchCspt
  return entryCount;
}

bool Dictionary::binarySearchCspt(HalFile& cspt, const char* target, uint32_t idxFileSize, uint32_t* startByte,
                                  uint32_t* endByte) {
  // Read and validate header (12 bytes).
  uint8_t hdr[CSPT_HEADER_SIZE];
  cspt.seekSet(0);
  if (cspt.read(hdr, CSPT_HEADER_SIZE) != static_cast<int>(CSPT_HEADER_SIZE)) return false;
  if (memcmp(hdr, CSPT_MAGIC, 4) != 0 || hdr[4] != CSPT_VERSION) return false;

  const uint8_t prefixLen = hdr[5];
  uint16_t stride;
  memcpy(&stride, hdr + 6, 2);  // LE
  uint32_t entryCount;
  memcpy(&entryCount, hdr + 8, 4);  // LE

  if (entryCount == 0 || prefixLen == 0 || prefixLen > 128) {
    *startByte = 0;
    *endByte = idxFileSize;
    return true;
  }

  const uint32_t entrySize = prefixLen + 4;

  // Binary search: find last entry whose prefix <= target (case-insensitive).
  uint32_t lo = 0, hi = entryCount - 1;
  uint8_t entry[128 + 4];  // prefixLen capped at 128 above

  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1) / 2;
    cspt.seekSet(CSPT_HEADER_SIZE + mid * entrySize);
    if (cspt.read(entry, entrySize) != static_cast<int>(entrySize)) return false;

    // Null-terminate prefix for cistrcmp (prefix is already null-padded if shorter).
    entry[prefixLen] = '\0';
    if (cistrcmp(reinterpret_cast<const char*>(entry), target) > 0) {
      hi = mid - 1;
    } else {
      lo = mid;
    }
  }

  // Read the matched entry to get idxOffset.
  cspt.seekSet(CSPT_HEADER_SIZE + lo * entrySize);
  if (cspt.read(entry, entrySize) != static_cast<int>(entrySize)) return false;

  uint32_t idxOffset;
  memcpy(&idxOffset, entry + prefixLen, 4);  // LE
  *startByte = idxOffset;

  // End bound: read next entry's idxOffset, or use idxFileSize for last entry.
  if (lo + 1 < entryCount) {
    cspt.seekSet(CSPT_HEADER_SIZE + (lo + 1) * entrySize);
    if (cspt.read(entry, entrySize) != static_cast<int>(entrySize)) return false;
    memcpy(endByte, entry + prefixLen, 4);  // LE
  } else {
    *endByte = idxFileSize;
  }

  return true;
}

// How many index pages either side of the landing page a widened sweep covers. Shared by
// locate()'s miss retry and findSimilar() so the exact lookup and the suggestion scan always
// consider the same region — them covering different amounts is the bug this constant exists
// to keep fixed (see the retry in locate()).
static constexpr uint32_t PAGE_RADIUS = 7;

// Byte offset where index page `page` begins. Page 0 always starts at 0; entry (page-1) of the
// .cspt/.oft records the start of page `page`. These recorded starts are the ONLY legal seek
// targets in a .idx — its entries are variable-length (null-terminated word + 8 bytes), so an
// arbitrary byte offset would land mid-record and desynchronise the reader.
static bool readIndexPageStart(HalFile& index, bool isCspt, uint32_t numEntries, uint32_t page, uint32_t* out) {
  if (page == 0) {
    *out = 0;
    return true;
  }
  if (page > numEntries) return false;
  const uint32_t pos =
      isCspt ? (CSPT_HEADER_SIZE + (page - 1) * CSPT_ENTRY_SIZE + CSPT_PREFIX_LEN) : (OFT_HEADER_SIZE + (page - 1) * 4);
  index.seekSet(pos);
  uint8_t raw[4];
  if (index.read(raw, 4) != 4) return false;
  memcpy(out, raw, sizeof(*out));  // both index formats store a little-endian uint32
  return true;
}

// Entry count a page index holds, given its format. Shared so the open-handle and
// open-by-path forms of widenScanBounds cannot drift apart.
static uint32_t pageIndexEntryCount(HalFile& index, bool isCspt) {
  const uint32_t sz = static_cast<uint32_t>(index.fileSize());
  if (isCspt) return sz > CSPT_HEADER_SIZE ? (sz - CSPT_HEADER_SIZE) / CSPT_ENTRY_SIZE : 0;
  return sz > OFT_HEADER_SIZE ? (sz - OFT_HEADER_SIZE) / 4 : 0;
}

// Expand a single-page window to PAGE_RADIUS pages either side of it, on real page boundaries.
// Defaults to the whole file when no page index exists (then it is already one page).
// Open-handle form: the caller already holds the page index, so the miss path no longer
// reopens the file resolveScanBounds just finished with.
static void widenScanBoundsIn(HalFile& index, bool isCspt, uint32_t srcFileSize, uint32_t centerStart,
                              uint32_t* outStart, uint32_t* outEnd) {
  *outStart = 0;
  *outEnd = srcFileSize;

  const uint32_t numEntries = pageIndexEntryCount(index, isCspt);
  if (numEntries == 0) return;

  // Page starts are monotonically increasing, so binary search for the page holding centerStart.
  uint32_t lo = 0;
  uint32_t hi = numEntries;
  uint32_t landed = 0;
  while (lo <= hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    uint32_t start = 0;
    if (!readIndexPageStart(index, isCspt, numEntries, mid, &start)) break;
    if (start <= centerStart) {
      landed = mid;
      lo = mid + 1;
    } else {
      if (mid == 0) break;
      hi = mid - 1;
    }
  }

  uint32_t bound = 0;
  if (readIndexPageStart(index, isCspt, numEntries, landed > PAGE_RADIUS ? landed - PAGE_RADIUS : 0, &bound)) {
    *outStart = bound;
  }
  // End of the last page in the window == start of the page after it; past the end, the file end.
  if (readIndexPageStart(index, isCspt, numEntries, landed + PAGE_RADIUS + 1, &bound)) {
    *outEnd = bound;
  }
}

void Dictionary::resolveScanBoundsIn(LookupCtx& ctx, const char* target, uint32_t* startByte, uint32_t* endByte) {
  if (!ctx.hasPageIndex) return;  // no index: caller's full-file bounds stand

  if (!ctx.pageIndexIsCspt) {
    findPageBounds(ctx.pageIndex, ctx.idx, ctx.idxSize, target, startByte, endByte);
    return;
  }

  if (binarySearchCspt(ctx.pageIndex, target, ctx.idxSize, startByte, endByte)) return;

  // .cspt is stale or malformed. The path-based form falls back to .oft here, and dropping
  // that would turn every probe into a full-file scan on a large dictionary — so keep it,
  // but open .oft lazily and once: the fallback costs nothing until a bad sidecar needs it.
  if (!ctx.oftTried) {
    ctx.oftTried = true;
    char path[160];
    if (buildPath(path, sizeof(path), ctx.base, ".idx.oft")) {
      ctx.hasOftFallback = Storage.openFileForRead("DICT", path, ctx.oftFallback);
    }
  }
  if (ctx.hasOftFallback) {
    findPageBounds(ctx.oftFallback, ctx.idx, ctx.idxSize, target, startByte, endByte);
  }
}

void Dictionary::resolveScanBounds(const char* csptPath, const char* oftPath, HalFile& src, uint32_t srcFileSize,
                                   const char* target, uint32_t* startByte, uint32_t* endByte) {
  // Try .cspt first (CrossPoint optimized index), fall back to .oft.
  bool boundsResolved = false;
  HalFile cspt;
  if (Storage.openFileForRead("DICT", csptPath, cspt)) {
    boundsResolved = binarySearchCspt(cspt, target, srcFileSize, startByte, endByte);
    cspt.close();
  }
  if (!boundsResolved) {
    HalFile oft;
    if (Storage.openFileForRead("DICT", oftPath, oft)) {
      findPageBounds(oft, src, srcFileSize, target, startByte, endByte);
      oft.close();
    }
  }
}

void Dictionary::findPageBounds(HalFile& oft, HalFile& src, uint32_t srcFileSize, const char* target,
                                uint32_t* startByte, uint32_t* endByte) {
  const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
  const uint32_t numEntries = (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;
  const uint32_t numPages = numEntries + 1;  // page 0 is implicit (starts at byte 0 in src)

  if (numEntries == 0) {
    // No OFT entries — entire source file is one page
    *startByte = 0;
    *endByte = srcFileSize;
    return;
  }

  // Returns the byte offset in src where page K begins.
  // Page 0 always starts at 0; page K>0 is stored in OFT entry K-1 (LE uint32).
  auto pageStart = [&](uint32_t k) -> uint32_t {
    if (k == 0) return 0;
    oft.seekSet(OFT_HEADER_SIZE + (k - 1) * 4);
    uint8_t raw[4];
    if (oft.read(raw, 4) != 4) return srcFileSize;
    uint32_t val;
    memcpy(&val, raw, 4);  // little-endian on ESP32-C3
    return val;
  };

  // Binary search: find the last page whose first word <= target
  uint32_t lo = 0, hi = numPages - 1;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1) / 2;
    uint32_t midStart = pageStart(mid);
    src.seekSet(midStart);
    int len = readWordInto(src, wordBuf, sizeof(wordBuf));
    if (len < 0 || cistrcmp(wordBuf, target) > 0) {
      hi = mid - 1;
    } else {
      lo = mid;
    }
  }

  *startByte = pageStart(lo);
  *endByte = (lo + 1 < numPages) ? pageStart(lo + 1) : srcFileSize;
}

// ---------------------------------------------------------------------------
// Locate (index search only — no definition read, zero RAM growth)
// ---------------------------------------------------------------------------

bool Dictionary::buildPath(char* buf, size_t bufSize, const char* base, const char* suffix) {
  const int n = snprintf(buf, bufSize, "%s%s", base, suffix);
  return n > 0 && static_cast<size_t>(n) < bufSize;
}

bool Dictionary::openLookupCtx(LookupCtx& ctx, const char* cachePath) {
  // One resolve for the whole probe sequence. activeDictPath() reads dictionary.bin off SD
  // whenever no session override is set, which the reader flow does not set — so this alone
  // was an SD open per probe.
  // "" still goes through openLookupCtxAt so the ctx is reset even on this path: callers read
  // ctx.base to tell "nothing configured" from "unreadable", and a reused ctx would otherwise
  // still be holding the previous dictionary's handles and path.
  const std::string folder = activeDictPath(cachePath);
  return openLookupCtxAt(ctx, folder.c_str());
}

bool Dictionary::openLookupCtxAt(LookupCtx& ctx, const char* basePath) {
  // Full reset, not just the validity flags. A ctx reopened against a DIFFERENT dictionary
  // would otherwise keep the previous one's lazily-opened .oft fallback and answer probes
  // from the wrong index, and would leave its old handles open for the ctx's lifetime.
  // Releasing is explicit here because these are members, not scope-local files.
  //
  // Move-assign a fresh handle rather than calling close(): HalFile is a pimpl, and close()
  // asserts impl != nullptr (HalStorage.cpp), so it PANICS on a never-opened handle — which
  // is exactly what a stack LookupCtx is on the first call. Assignment also frees the Impl,
  // which close() alone does not; the old Impl's destructor takes StorageLock and closes the
  // underlying FsFile.
  ctx.idx = HalFile();
  ctx.pageIndex = HalFile();
  ctx.oftFallback = HalFile();
  ctx.valid = false;
  ctx.hasPageIndex = false;
  ctx.pageIndexIsCspt = false;
  ctx.oftTried = false;
  ctx.hasOftFallback = false;
  ctx.idxSize = 0;
  ctx.base[0] = '\0';

  if (basePath == nullptr || basePath[0] == '\0') return false;
  if (!buildPath(ctx.base, sizeof(ctx.base), basePath, "")) {
    // snprintf still wrote a truncated path; blank it so callers never see a partial one.
    // 128 is the same ceiling setSessionDictPath and readDictPath already impose.
    LOG_ERR("DICT", "Dictionary path too long: %s", basePath);
    ctx.base[0] = '\0';
    return false;
  }

  char path[160];
  if (!buildPath(path, sizeof(path), ctx.base, ".idx")) return false;
  if (!Storage.openFileForRead("DICT", path, ctx.idx)) return false;  // base is set: unreadable, not absent
  ctx.idxSize = static_cast<uint32_t>(ctx.idx.fileSize());

  // Page index is optional — without it locate() falls back to a full scan. .cspt first,
  // matching resolveScanBounds' preference order.
  if (buildPath(path, sizeof(path), ctx.base, ".idx.oft.cspt") &&
      Storage.openFileForRead("DICT", path, ctx.pageIndex)) {
    ctx.pageIndexIsCspt = true;
    ctx.hasPageIndex = true;
  } else if (buildPath(path, sizeof(path), ctx.base, ".idx.oft") &&
             Storage.openFileForRead("DICT", path, ctx.pageIndex)) {
    ctx.pageIndexIsCspt = false;
    ctx.hasPageIndex = true;
  }

  ctx.valid = true;
  return true;
}

DictLocation Dictionary::locate(const std::string& word, const DictLookupCallbacks& cbs, const char* cachePath) {
  LookupCtx ctx;
  if (!openLookupCtx(ctx, cachePath)) {
    DictLocation result;
    // base is filled only when the dictionary resolved but its .idx would not open, which
    // is what separates "nothing configured" from "configured but broken".
    result.status = ctx.base[0] == '\0' ? LookupStatus::NoDictionary : LookupStatus::ReadError;
    result.folderPath = ctx.base;  // "" when no dictionary is configured
    return result;
  }
  return locateIn(ctx, word, cbs);
}

DictLocation Dictionary::locateIn(LookupCtx& ctx, const std::string& word, const DictLookupCallbacks& cbs) {
  DictLocation result;
  if (!ctx.valid) {
    result.status = ctx.base[0] == '\0' ? LookupStatus::NoDictionary : LookupStatus::ReadError;
    return result;
  }
  result.folderPath = ctx.base;

  HalFile& idx = ctx.idx;
  const uint32_t idxFileSize = ctx.idxSize;
  uint32_t startByte = 0;
  uint32_t endByte = idxFileSize;

  resolveScanBoundsIn(ctx, word.c_str(), &startByte, &endByte);

  if (cbs.onProgress) cbs.onProgress(cbs.ctx, 70);

  // No idx.close() on any exit below: the handle belongs to the ctx and the next probe in a
  // stem sequence reuses it. It closes with the ctx (DESTRUCTOR_CLOSES_FILE=1).
  IdxScanner scan(idx, startByte);
  while (scan.position() < endByte) {
    if (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx)) return result;

    int len = readWordInto(scan, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t suffix[8];
    if (!scan.readBytes(suffix, 8)) break;

    int cmp = cistrcmp(wordBuf, word.c_str());
    if (cmp == 0) {
      result.offset = (static_cast<uint32_t>(suffix[0]) << 24) | (static_cast<uint32_t>(suffix[1]) << 16) |
                      (static_cast<uint32_t>(suffix[2]) << 8) | static_cast<uint32_t>(suffix[3]);
      result.size = (static_cast<uint32_t>(suffix[4]) << 24) | (static_cast<uint32_t>(suffix[5]) << 16) |
                    (static_cast<uint32_t>(suffix[6]) << 8) | static_cast<uint32_t>(suffix[7]);
      result.found = true;
      result.status = LookupStatus::Found;
      if (cbs.onProgress) cbs.onProgress(cbs.ctx, 100);
      return result;
    }

    if (cmp > 0) break;
  }

  if (!result.found) {
    // Both exits above are collation-dependent, and both are wrong for a dictionary that is not
    // sorted the way cistrcmp compares:
    //   1. resolveScanBounds picked ONE page using cistrcmp, so a differently-sorted index lands
    //      the search a page off and the word is never scanned at all;
    //   2. the `cmp > 0` break abandons the page early once cistrcmp thinks it has passed the
    //      target, so even the RIGHT page can be given up on before reaching the entry.
    // cistrcmp assumes case-insensitive ordering, true of the wiktionary-derived dictionaries it
    // was written for. CC-CEDICT-derived Chinese dictionaries are commonly byte-sorted and mix
    // ASCII pinyin with CJK headwords — precisely where the two orders diverge. The visible
    // symptom is "Did you mean?" offering neighbours of a word that is plainly in the dictionary,
    // because findSimilar's PAGE_RADIUS sweep finds what this scan just missed.
    //
    // So retry over that same window, comparing for equality only — no ordering shortcut, since
    // it is the ordering assumption that failed. Runs on misses only, and a miss was about to
    // scan this exact region through findSimilar anyway.
    uint32_t wideStart = 0;
    uint32_t wideEnd = idxFileSize;
    // Prefer the .oft fallback when one is open: it exists only because the .cspt search
    // already failed, and widening against a sidecar known to be bad reads garbage page
    // starts. (The old path-based widenScanBounds always reopened the .cspt and did exactly
    // that; holding both handles is what makes the better choice free.)
    if (ctx.hasOftFallback) {
      widenScanBoundsIn(ctx.oftFallback, /*isCspt=*/false, idxFileSize, startByte, &wideStart, &wideEnd);
    } else if (ctx.hasPageIndex) {
      widenScanBoundsIn(ctx.pageIndex, ctx.pageIndexIsCspt, idxFileSize, startByte, &wideStart, &wideEnd);
    }

    if (wideStart < startByte || wideEnd > endByte) {
      // Every byte of this window is read on a miss, so its size IS the cost of the retry.
      // Reported whether or not it hits: a wide scan that finds nothing is silent otherwise,
      // and that silent case is the one that would make lookups feel slower.
      //
      // Bytes rather than milliseconds: no millis() on the host, where this file is compiled
      // by the dict-lookup-session suite, and the controller's "miss" line already times the
      // whole miss. The window size is the cost driver and is deterministic per dictionary.
      const uint32_t wideBytes = wideEnd - wideStart;
      // Logged BEFORE the scan, not just after it. The miss/hit lines below only ever appear
      // once the loop has finished, so a scan that never finishes is invisible — and this loop
      // is the leading suspect for the "stuck, had to power-cycle" report: when widening cannot
      // narrow the window it stays at wideEnd = idxFileSize, i.e. a byte-at-a-time pass over the
      // WHOLE index. If that hang recurs, this is the last line written and its wideBytes says
      // whether the window was the cause. Cheap: one SD log line per miss, against a scan that
      // is already reading kilobytes.
      //
      // No yield in the loop below: the lookup task and the UI task both run at priority 1 and
      // the build has configUSE_TIME_SLICING=1 at configTICK_RATE_HZ=1000, so equal-priority
      // round-robin already preempts this every tick. A vTaskDelay here would buy no
      // responsiveness and would not compile on the host, where this file is built by the
      // dict-lookup-session suite.
      SdDebugLog::log("DICT", "locate: widened scan start, window %u-%u (%uB) for '%s'", (unsigned)wideStart,
                      (unsigned)wideEnd, (unsigned)wideBytes, word.c_str());
      scan.seek(wideStart);
      while (scan.position() < wideEnd) {
        if (cbs.shouldCancel && cbs.shouldCancel(cbs.ctx)) break;
        const int len = readWordInto(scan, wordBuf, sizeof(wordBuf));
        if (len < 0) break;
        uint8_t suffix[8];
        if (!scan.readBytes(suffix, 8)) break;
        if (len == 0 || cistrcmp(wordBuf, word.c_str()) != 0) continue;

        result.offset = (static_cast<uint32_t>(suffix[0]) << 24) | (static_cast<uint32_t>(suffix[1]) << 16) |
                        (static_cast<uint32_t>(suffix[2]) << 8) | static_cast<uint32_t>(suffix[3]);
        result.size = (static_cast<uint32_t>(suffix[4]) << 24) | (static_cast<uint32_t>(suffix[5]) << 16) |
                      (static_cast<uint32_t>(suffix[6]) << 8) | static_cast<uint32_t>(suffix[7]);
        result.found = true;
        result.status = LookupStatus::Found;
        // Logged to SD because it is the signal that a dictionary's sort order disagrees with
        // cistrcmp: a hit here is one the single-page scan should have found and did not.
        SdDebugLog::log("DICT", "locate: widened scan hit, page window %u-%u vs %u-%u, %uB", (unsigned)wideStart,
                        (unsigned)wideEnd, (unsigned)startByte, (unsigned)endByte, (unsigned)wideBytes);
        break;
      }
      if (!result.found) {
        SdDebugLog::log("DICT", "locate: widened scan miss, %uB", (unsigned)wideBytes);
      }
    }
  }

  if (cbs.onProgress) cbs.onProgress(cbs.ctx, 100);
  return result;
}

// ---------------------------------------------------------------------------
// Alternate-form lookup (.syn)
// ---------------------------------------------------------------------------

// Resolve the word at 0-based ordinal in .idx using .idx.oft for fast page seek.
std::string Dictionary::wordAtOrdinal(const std::string& folderPath, uint32_t ordinal) {
  DictPaths dp(folderPath);
  HalFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return "";

  const uint32_t pageNum = ordinal / OFT_STRIDE;
  const uint32_t withinPage = ordinal % OFT_STRIDE;
  uint32_t pageStartByte = 0;

  if (pageNum > 0) {
    HalFile oft;
    if (Storage.openFileForRead("DICT", dp.idxOft().c_str(), oft)) {
      oft.seekSet(OFT_HEADER_SIZE + (pageNum - 1) * 4);
      uint8_t raw[4];
      if (oft.read(raw, 4) == 4) memcpy(&pageStartByte, raw, 4);  // LE uint32
      oft.close();
    }
  }

  IdxScanner scan(idx, pageStartByte);

  // Skip `withinPage` entries to reach the target
  for (uint32_t i = 0; i < withinPage; i++) {
    if (readWordInto(scan, wordBuf, sizeof(wordBuf)) < 0) {
      idx.close();
      return "";
    }
    uint8_t skip[8];
    if (!scan.readBytes(skip, 8)) {
      idx.close();
      return "";
    }
  }

  int len = readWordInto(scan, wordBuf, sizeof(wordBuf));
  idx.close();
  if (len < 0) return "";
  return std::string(wordBuf, static_cast<size_t>(len));
}

std::string Dictionary::resolveAltForm(const std::string& word, const char* cachePath) {
  std::string folderPath = activeDictPath(cachePath);
  if (folderPath.empty()) return "";

  DictPaths dp(folderPath);
  if (!Storage.exists(dp.syn().c_str())) return "";

  HalFile syn;
  if (!Storage.openFileForRead("DICT", dp.syn().c_str(), syn)) return "";

  const uint32_t synFileSize = static_cast<uint32_t>(syn.fileSize());
  uint32_t startByte = 0;
  uint32_t endByte = synFileSize;

  resolveScanBounds(dp.synOftCspt().c_str(), dp.synOft().c_str(), syn, synFileSize, word.c_str(), &startByte, &endByte);

  // The .idx ordinal the matching .syn entry points at; -1 until the scan finds one.
  // Resolved AFTER the scanner is gone, not inside the loop: wordAtOrdinal opens a scanner of
  // its own, and IdxScanner shares one static window (as wordBuf does), so two live scanners
  // must never both be read from.
  int64_t originalIdx = -1;
  {
    IdxScanner scan(syn, startByte);
    while (scan.position() < endByte) {
      int len = readWordInto(scan, wordBuf, sizeof(wordBuf));
      if (len < 0) break;

      uint8_t idxBuf[4];
      if (!scan.readBytes(idxBuf, 4)) break;

      int cmp = cistrcmp(wordBuf, word.c_str());
      if (cmp == 0) {
        // Big-endian original word index in .idx
        originalIdx = (static_cast<uint32_t>(idxBuf[0]) << 24) | (static_cast<uint32_t>(idxBuf[1]) << 16) |
                      (static_cast<uint32_t>(idxBuf[2]) << 8) | static_cast<uint32_t>(idxBuf[3]);
        break;
      }

      if (cmp > 0) break;
    }
  }

  syn.close();
  if (originalIdx < 0) return "";
  return wordAtOrdinal(folderPath, static_cast<uint32_t>(originalIdx));
}

// ---------------------------------------------------------------------------
// Stemming
// ---------------------------------------------------------------------------

std::vector<std::string> Dictionary::getStemVariants(const std::string& word) {
  std::vector<std::string> variants;
  variants.reserve(8);
  size_t len = word.size();
  if (len < 3) return variants;

  auto endsWith = [&word, len](const char* suffix) {
    size_t slen = strlen(suffix);
    return len >= slen && word.compare(len - slen, slen, suffix) == 0;
  };

  auto add = [&variants](const std::string& s) {
    if (s.size() >= 2) variants.push_back(s);
  };

  // Plurals
  if (endsWith("sses")) add(word.substr(0, len - 2));
  if (endsWith("ses")) add(word.substr(0, len - 2) + "is");
  if (endsWith("ies")) {
    add(word.substr(0, len - 3) + "y");
    add(word.substr(0, len - 2));
  }
  if (endsWith("ves")) {
    add(word.substr(0, len - 3) + "f");
    add(word.substr(0, len - 3) + "fe");
    add(word.substr(0, len - 1));
  }
  if (endsWith("men")) add(word.substr(0, len - 3) + "man");
  if (endsWith("es") && !endsWith("sses") && !endsWith("ies") && !endsWith("ves")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 1));
  }
  if (endsWith("s") && !endsWith("ss") && !endsWith("us") && !endsWith("es")) {
    add(word.substr(0, len - 1));
  }

  // Past tense
  if (endsWith("ied")) {
    add(word.substr(0, len - 3) + "y");
    add(word.substr(0, len - 1));
  }
  if (endsWith("ed") && !endsWith("ied")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 1));
    if (len > 4 && word[len - 3] == word[len - 4]) {
      add(word.substr(0, len - 3));
    }
  }

  // Progressive
  if (endsWith("ying")) {
    add(word.substr(0, len - 4) + "ie");
  }
  if (endsWith("ing") && !endsWith("ying")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
    if (len > 5 && word[len - 4] == word[len - 5]) {
      add(word.substr(0, len - 4));
    }
  }

  // Adverb
  if (endsWith("ically")) {
    add(word.substr(0, len - 6) + "ic");
    add(word.substr(0, len - 4));
  }
  if (endsWith("ally") && !endsWith("ically")) {
    add(word.substr(0, len - 4) + "al");
    add(word.substr(0, len - 2));
  }
  if (endsWith("ily") && !endsWith("ally")) {
    add(word.substr(0, len - 3) + "y");
  }
  if (endsWith("ly") && !endsWith("ily") && !endsWith("ally")) {
    add(word.substr(0, len - 2));
  }

  // Comparative / superlative
  if (endsWith("ier")) {
    add(word.substr(0, len - 3) + "y");
  }
  if (endsWith("er") && !endsWith("ier")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 1));
    if (len > 4 && word[len - 3] == word[len - 4]) {
      add(word.substr(0, len - 3));
    }
  }
  if (endsWith("iest")) {
    add(word.substr(0, len - 4) + "y");
  }
  if (endsWith("est") && !endsWith("iest")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 2));
    if (len > 5 && word[len - 4] == word[len - 5]) {
      add(word.substr(0, len - 4));
    }
  }

  // Derivational suffixes
  if (endsWith("ness")) add(word.substr(0, len - 4));
  if (endsWith("ment")) add(word.substr(0, len - 4));
  if (endsWith("ful")) add(word.substr(0, len - 3));
  if (endsWith("less")) add(word.substr(0, len - 4));
  if (endsWith("able")) {
    add(word.substr(0, len - 4));
    add(word.substr(0, len - 4) + "e");
  }
  if (endsWith("ible")) {
    add(word.substr(0, len - 4));
    add(word.substr(0, len - 4) + "e");
  }
  if (endsWith("ation")) {
    add(word.substr(0, len - 5));
    add(word.substr(0, len - 5) + "e");
    add(word.substr(0, len - 5) + "ate");
  }
  if (endsWith("tion") && !endsWith("ation")) {
    add(word.substr(0, len - 4) + "te");
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ion") && !endsWith("tion")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("al") && !endsWith("ial")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 2) + "e");
  }
  if (endsWith("ial")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ous")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ive")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ize")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("ise")) {
    add(word.substr(0, len - 3));
    add(word.substr(0, len - 3) + "e");
  }
  if (endsWith("en")) {
    add(word.substr(0, len - 2));
    add(word.substr(0, len - 2) + "e");
  }

  // Prefix removal
  if (len > 5 && word.compare(0, 2, "un") == 0) add(word.substr(2));
  if (len > 6 && word.compare(0, 3, "dis") == 0) add(word.substr(3));
  if (len > 6 && word.compare(0, 3, "mis") == 0) add(word.substr(3));
  if (len > 6 && word.compare(0, 3, "pre") == 0) add(word.substr(3));
  if (len > 7 && word.compare(0, 4, "over") == 0) add(word.substr(4));
  if (len > 5 && word.compare(0, 2, "re") == 0) add(word.substr(2));

  // Deduplicate preserving insertion order
  std::vector<std::string> deduped;
  deduped.reserve(variants.size());
  std::copy_if(variants.begin(), variants.end(), std::back_inserter(deduped), [&deduped](const std::string& v) {
    return std::find(deduped.begin(), deduped.end(), v) == deduped.end();
  });
  return deduped;
}

// ---------------------------------------------------------------------------
// Fuzzy search (zero persistent RAM — uses findPageBounds for neighbourhood)
// ---------------------------------------------------------------------------

int Dictionary::editDistance(const std::string& a, const std::string& b, int maxDist) {
  int m = static_cast<int>(a.size());
  int n = static_cast<int>(b.size());
  if (std::abs(m - n) > maxDist) return maxDist + 1;

  std::vector<int> dp(n + 1);
  for (int j = 0; j <= n; j++) dp[j] = j;

  for (int i = 1; i <= m; i++) {
    int prev = dp[0];
    dp[0] = i;
    int rowMin = dp[0];
    for (int j = 1; j <= n; j++) {
      int temp = dp[j];
      if (a[i - 1] == b[j - 1]) {
        dp[j] = prev;
      } else {
        dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
      }
      prev = temp;
      if (dp[j] < rowMin) rowMin = dp[j];
    }
    if (rowMin > maxDist) return maxDist + 1;
  }
  return dp[n];
}

std::vector<std::string> Dictionary::findSimilar(const std::string& word, int maxResults, const char* cachePath) {
  std::string folderPath = activeDictPath(cachePath);
  if (folderPath.empty()) return {};

  DictPaths dp(folderPath);
  HalFile idx;
  if (!Storage.openFileForRead("DICT", dp.idx().c_str(), idx)) return {};

  const uint32_t idxFileSize = static_cast<uint32_t>(idx.fileSize());
  uint32_t centerStart = 0;
  uint32_t centerEnd = idxFileSize;

  HalFile oft;
  const bool hasOft = Storage.openFileForRead("DICT", dp.idxOft().c_str(), oft);

  if (hasOft) {
    findPageBounds(oft, idx, idxFileSize, word.c_str(), &centerStart, &centerEnd);
  }

  // Extend the scan window by ±7 pages around the found neighbourhood page.
  // Each page is approximately (centerEnd - centerStart) bytes.
  const uint32_t pageSize = (centerEnd > centerStart) ? (centerEnd - centerStart) : 1;
  const uint32_t scanStart = (centerStart > PAGE_RADIUS * pageSize) ? (centerStart - PAGE_RADIUS * pageSize) : 0;
  const uint32_t scanEnd = std::min(idxFileSize, centerEnd + PAGE_RADIUS * pageSize);

  uint32_t scanFrom = scanStart;

  if (hasOft) {
    // Snap scanStart back to the true page boundary containing it via binary search.
    // Re-use findPageBounds with the first word of the scan region as target would be complex;
    // instead just clamp to a page-aligned position by seeking to the OFT entry.
    // For simplicity, snap to the nearest OFT page boundary at or before scanStart.
    const uint32_t oftFileSize = static_cast<uint32_t>(oft.fileSize());
    const uint32_t numEntries = (oftFileSize > OFT_HEADER_SIZE) ? (oftFileSize - OFT_HEADER_SIZE) / 4 : 0;

    // Walk backward through OFT entries to find the largest entry <= scanStart
    uint32_t snappedStart = 0;
    for (uint32_t i = 0; i < numEntries; i++) {
      oft.seekSet(OFT_HEADER_SIZE + i * 4);
      uint8_t raw[4];
      if (oft.read(raw, 4) != 4) break;
      uint32_t entryVal;
      memcpy(&entryVal, raw, 4);
      if (entryVal <= scanStart) {
        snappedStart = entryVal;
      } else {
        break;  // OFT entries are monotonically increasing
      }
    }
    oft.close();

    scanFrom = snappedStart;
  }

  int maxDist = std::max(2, static_cast<int>(word.size()) / 3 + 1);

  struct Candidate {
    std::string text;
    int distance;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(static_cast<size_t>(maxResults) * 4);

  IdxScanner scan(idx, scanFrom);
  while (scan.position() < scanEnd) {
    int len = readWordInto(scan, wordBuf, sizeof(wordBuf));
    if (len < 0) break;

    uint8_t skip[8];
    if (!scan.readBytes(skip, 8)) break;

    if (len == 0) continue;
    if (cistrcmp(wordBuf, word.c_str()) == 0) continue;

    int dist = editDistance(wordBuf, word, maxDist);
    if (dist <= maxDist) {
      candidates.push_back({std::string(wordBuf, static_cast<size_t>(len)), dist});
    }
  }

  idx.close();

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; });

  std::vector<std::string> results;
  results.reserve(static_cast<size_t>(maxResults));
  for (size_t i = 0; i < candidates.size() && static_cast<int>(results.size()) < maxResults; i++) {
    results.push_back(candidates[i].text);
  }
  return results;
}
