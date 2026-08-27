#include "LookupMarks.h"

#include <Logging.h>
#include <Memory.h>

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

}  // namespace

uint32_t LookupMarks::hashAppend(uint32_t h, const char* text, const size_t len, uint16_t* inOutLen) {
  if (!text) return h;
  uint16_t kept = 0;
  for (size_t i = 0; i < len; i++) {
    auto b = static_cast<unsigned char>(text[i]);
    if (b < 0x80) {
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
  if (!marks_ || count_ == 0 || !out || cap <= 0 || page <= 0) return 0;
  int found = 0;
  for (int i = 0; i < count_ && found < cap; i++) {
    const Mark& m = marks_[i];
    if (m.page != page || m.pageCount != pageCount || m.chapterHash != chapterHash) continue;
    out[found++] = &m;
  }
  return found;
}
