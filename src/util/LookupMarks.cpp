#include "LookupMarks.h"

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

namespace {

// Bytes of the leading UTF-8 codepoint. A malformed leading byte counts as one, which is what
// every other walk in the firmware does with one.
size_t firstCodepointLen(const char* text, size_t len) {
  if (len == 0) return 0;
  const auto b = static_cast<unsigned char>(text[0]);
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return len >= 2 ? 2 : 1;
  if ((b & 0xF0) == 0xE0) return len >= 3 ? 3 : 1;
  if ((b & 0xF8) == 0xF0) return len >= 4 ? 4 : 1;
  return 1;
}

// Codepoint of the multi-byte sequence at text[0..n), n from firstCodepointLen. 0 when n is 1
// (a malformed lead byte), which no punctuation test matches.
uint32_t decodeCodepoint(const char* text, const size_t n) {
  const auto* b = reinterpret_cast<const unsigned char*>(text);
  if (n == 2) return ((b[0] & 0x1Fu) << 6) | (b[1] & 0x3Fu);
  if (n == 3) return ((b[0] & 0x0Fu) << 12) | ((b[1] & 0x3Fu) << 6) | (b[2] & 0x3Fu);
  if (n == 4) return ((b[0] & 0x07u) << 18) | ((b[1] & 0x3Fu) << 12) | ((b[2] & 0x3Fu) << 6) | (b[3] & 0x3Fu);
  return 0;
}

bool isSkippedPunctuation(const uint32_t cp) {
  return utf8IsCjkPunctuation(cp) || (cp >= 0x2010 && cp <= 0x2027) || cp == 0x00B7;
}

}  // namespace

uint32_t LookupMarks::hashAppend(uint32_t h, const char* text, const size_t len, uint16_t* inOutLen) {
  if (!text) return h;
  uint16_t kept = 0;
  for (size_t i = 0; i < len; i++) {
    auto b = static_cast<unsigned char>(text[i]);
    if (b >= 0x80) {
      const size_t n = firstCodepointLen(text + i, len - i);
      if (n > 1 && isSkippedPunctuation(decodeCodepoint(text + i, n))) {
        i += n - 1;
        continue;
      }
    } else {
      if (b >= 'A' && b <= 'Z') {
        b = static_cast<unsigned char>(b + ('a' - 'A'));
      } else if (!((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9'))) {
        continue;  // ASCII punctuation and spaces carry no identity
      }
    }
    h ^= b;
    h *= 16777619u;
    kept++;
  }
  if (inOutLen) *inOutLen = static_cast<uint16_t>(*inOutLen + kept);
  return h;
}

LookupMarks::Step LookupMarks::step(const Mark& m, RunState& r, const bool isCjk, const uint32_t tokenHash,
                                    const uint16_t tokenLen, const char* text, const size_t len, const int16_t rowY) {
  if (!isCjk) {
    // One token, one word: the whole identity is here or it is not.
    return (tokenHash == m.wordHash && tokenLen == m.byteLen) ? Step::MatchedToken : Step::None;
  }

  r.wrappedHere = false;
  if (r.open && r.y != rowY) {
    if (r.wrapped) {
      r.open = false;  // a second line break: no mark, no guess
    } else {
      r.wrapped = true;
      r.wrappedHere = true;
      r.y = rowY;
    }
  }

  const bool opening = !r.open;
  if (opening) {
    if (tokenHash != m.headHash) return Step::None;
    r.open = true;
    r.hash = tokenHash;
    r.len = tokenLen;
    r.y = rowY;
    r.wrapped = false;
  } else {
    r.hash = hashAppend(r.hash, text, len, &r.len);
  }

  if (r.len < m.byteLen) return opening ? Step::Opened : Step::Extended;

  // Complete, or overshot the word's length -- either way the run is done.
  const bool matched = r.len == m.byteLen && r.hash == m.wordHash;
  r.open = false;
  if (!matched) return opening ? Step::Opened : Step::Extended;
  // A one-character CJK word opens and completes on the same token, so its span is that token
  // rather than a run the caller has had a chance to seed.
  return opening ? Step::MatchedToken : Step::MatchedRun;
}

void LookupMarks::clear() {
  marks_.reset();
  count_ = 0;
  writeIdx_ = 0;
}

bool LookupMarks::add(const char* word, const int wordLen, const char* chapterTitle, const int titleLen, const int page,
                      const int pageCount) {
  // No page anchor, no mark: a legacy card, one synced from a peer, or one whose chapter title
  // was long enough that the " X/Y" token was cut off by the deck's chapter cap.
  if (!word || wordLen <= 0 || page <= 0 || pageCount <= 0) return false;
  if (page > UINT16_MAX || pageCount > UINT16_MAX) return false;

  if (!marks_) {
    marks_ = makeUniqueNoThrow<Mark[]>(MAX_MARKS);
    if (!marks_) {
      LOG_ERR("LMK", "OOM: %d marks", MAX_MARKS);
      return false;
    }
  }

  uint16_t byteLen = 0;
  const uint32_t wordHash = hashAppend(FNV_OFFSET, word, static_cast<size_t>(wordLen), &byteLen);
  if (byteLen == 0) return false;  // nothing but punctuation

  const size_t headLen = firstCodepointLen(word, static_cast<size_t>(wordLen));
  Mark& m = marks_[writeIdx_];
  m.chapterHash = hashChapter(chapterTitle, titleLen > 0 ? static_cast<size_t>(titleLen) : 0);
  m.wordHash = wordHash;
  m.headHash = hashWord(word, headLen);
  m.page = static_cast<uint16_t>(page);
  m.pageCount = static_cast<uint16_t>(pageCount);
  m.byteLen = byteLen;

  writeIdx_ = (writeIdx_ + 1) % MAX_MARKS;
  if (count_ < MAX_MARKS) count_++;
  return true;
}

int LookupMarks::collectForPage(const uint32_t chapterHash, const int page, const int pageCount, const Mark** out,
                                const int cap) const {
  if (!marks_ || count_ == 0 || !out || cap <= 0 || page <= 0 || pageCount <= 0) return 0;
  int found = 0;
  for (int i = 0; i < count_ && found < cap; i++) {
    const Mark& m = marks_[i];
    if (m.chapterHash != chapterHash) continue;
    // The same page number, or slices of the chapter that overlap: [(m.page-1)/m.pageCount,
    // m.page/m.pageCount) against the same for this page, cross-multiplied. The first holds
    // whenever the layout is unchanged, even though a total recorded mid-build was an estimate;
    // the second follows the word through a re-layout.
    const uint32_t mp = m.page, mc = m.pageCount, p = static_cast<uint32_t>(page), c = static_cast<uint32_t>(pageCount);
    if (mp != p && !((mp - 1) * c < p * mc && (p - 1) * mc < mp * c)) continue;
    out[found++] = &m;
  }
  return found;
}
