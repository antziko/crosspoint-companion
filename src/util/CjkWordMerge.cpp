#include "CjkWordMerge.h"

#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "util/TextPool.h"

namespace CjkWordMerge {

namespace {

using WordInfo = WordSelectNavigator::WordInfo;
using Row = WordSelectNavigator::Row;

// Longest joined text a merged token carries. A scan word is at most 8 cells.
constexpr size_t kMaxJoinedBytes = 96;

// Joins the display (or lookup) text of tokens [lo, hi] into buf. False when it does not fit.
bool joinText(const std::vector<WordInfo>& words, const std::string& pool, const int lo, const int hi,
              const bool lookup, char (&buf)[kMaxJoinedBytes], size_t& len) {
  len = 0;
  for (int i = lo; i <= hi; i++) {
    const WordInfo& w = words[i];
    // Each half of a word split over two rows carries the WHOLE word as its lookup; joining
    // those would repeat it, so a half contributes its own text.
    const bool half = w.continuationIndex >= 0 || w.continuationOf >= 0;
    const bool useLookup = lookup && !half;
    const uint16_t off = useLookup ? w.lookupOffset : w.textOffset;
    const uint16_t n = useLookup ? w.lookupLen : w.textLen;
    if (len + n >= kMaxJoinedBytes) return false;
    memcpy(buf + len, pool.data() + off, n);
    len += n;
  }
  buf[len] = '\0';
  return true;
}

// Byte offset of codepoint `n` in s[0, len), or len.
size_t codepointOffset(const char* s, const size_t len, const size_t n) {
  size_t seen = 0;
  for (size_t i = 0; i < len; i++) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
    if (seen++ == n) return i;
  }
  return len;
}

// Keeps codepoints [skip, skip + keep) of buf.
void sliceCodepoints(char (&buf)[kMaxJoinedBytes], size_t& len, const size_t skip, const size_t keep) {
  const size_t from = codepointOffset(buf, len, skip);
  const size_t to = from + codepointOffset(buf + from, len - from, keep);
  memmove(buf, buf + from, to - from);
  len = to - from;
  buf[len] = '\0';
}

// Leading codepoints of a token that carry no letter or digit: the opening quote glued to the
// character after it (“出).
size_t leadingMarks(const char* s, const size_t len) {
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  const auto* end = p + len;
  size_t n = 0;
  while (p < end) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (cp < 0x80 ? std::isalnum(static_cast<int>(cp)) != 0
                  : !utf8IsCjkPunctuation(cp) && cp != 0x2018 && cp != 0x201C) {
      break;
    }
    n++;
  }
  return n;
}

}  // namespace

bool merge(const std::vector<WordInfo>& words, const std::vector<Row>& rows, const std::string& pool,
           const std::vector<TokenGlyphs>& glyphs, const size_t cellCount, const std::vector<ScanWord>& scanWords,
           const bool vertical, std::vector<WordInfo>& outWords, std::vector<Row>& outRows, std::string& outPool,
           std::vector<Span>& outSpans) {
  outWords.clear();
  outRows.clear();
  outPool.clear();
  outSpans.clear();
  const int n = static_cast<int>(words.size());
  if (n == 0 || glyphs.size() != words.size()) return false;

  // groupEnd[t] = last token of the word starting at token t; -1 elsewhere. claimed marks every
  // token inside a word, so overlapping scan words cannot both take it.
  std::vector<int16_t> cellToken;
  std::vector<int16_t> groupEnd;
  std::vector<uint8_t> claimed;
  // For the group starting at token t: the word's length in cells, and the marks before it in t.
  std::vector<uint8_t> wordCells;
  std::vector<uint8_t> wordLead;
  if (!reserveNoThrow(cellToken, cellCount) || !reserveNoThrow(groupEnd, n) || !reserveNoThrow(claimed, n) ||
      !reserveNoThrow(wordCells, n) || !reserveNoThrow(wordLead, n)) {
    return false;
  }
  cellToken.assign(cellCount, -1);
  groupEnd.assign(n, -1);
  claimed.assign(n, 0);
  wordCells.assign(n, 0);
  wordLead.assign(n, 0);
  for (int t = 0; t < n; t++) {
    const TokenGlyphs& g = glyphs[t];
    if (g.start == TokenGlyphs::kNoGlyph) continue;
    for (size_t c = g.start; c < static_cast<size_t>(g.start) + g.count && c < cellCount; c++) {
      cellToken[c] = static_cast<int16_t>(t);
    }
  }

  int groups = 0;
  for (const ScanWord& sw : scanWords) {
    if (sw.length < 2 || sw.start >= cellCount) continue;
    const size_t end = std::min<size_t>(cellCount, static_cast<size_t>(sw.start) + sw.length);
    const int first = cellToken[sw.start];
    if (first < 0 || claimed[first]) continue;
    // A word may start past its token's opening marks (“出版), never mid-letter.
    const size_t lead = sw.start - glyphs[first].start;
    if (lead > 0 && lead > leadingMarks(pool.data() + words[first].textOffset, words[first].textLen)) continue;
    int last = first;
    bool ok = true;
    while (static_cast<size_t>(glyphs[last].start) + glyphs[last].count < end) {
      const int next = last + 1;
      if (next >= n || claimed[next] || glyphs[next].start == TokenGlyphs::kNoGlyph ||
          glyphs[next].start != glyphs[last].start + glyphs[last].count) {
        ok = false;
        break;
      }
      last = next;
    }
    if (!ok || last == first) continue;
    // Two rows at most: the continuation pair links exactly two halves.
    int rowBreaks = 0;
    for (int t = first + 1; t <= last; t++) {
      if (words[t].row != words[t - 1].row) rowBreaks++;
    }
    if (rowBreaks > 1) continue;
    for (int t = first; t <= last; t++) claimed[t] = 1;
    groupEnd[first] = static_cast<int16_t>(last);
    wordCells[first] = static_cast<uint8_t>(end - sw.start);
    wordLead[first] = static_cast<uint8_t>(lead);
    groups++;
  }

  // Each merged token's text is no longer than its members' put together, and a word split over
  // two rows adds one shared lookup string no longer than the pool itself.
  const size_t poolBytes = pool.size() + (groups > 0 ? pool.size() : 0) + 1;
  if (!reserveNoThrow(outWords, n) || !reserveNoThrow(outSpans, n) || !reserveNoThrow(outRows, rows.size())) {
    return false;
  }
  {
    void* probe = ::operator new(poolBytes + 1, std::nothrow);
    if (!probe) {
      outWords.clear();
      outSpans.clear();
      return false;
    }
    ::operator delete(probe, std::nothrow);
    outPool.reserve(poolBytes);
  }

  // Original -> output index, for continuation links between unmerged tokens (hyphen pairs).
  std::vector<int16_t> origToOut;
  if (!reserveNoThrow(origToOut, n)) return false;
  origToOut.assign(n, -1);

  bool failed = false;
  auto appendText = [&](const char* s, const size_t len, uint16_t& off) {
    if (!TextPool::appendNoThrow(outPool, s, len, off)) failed = true;
  };

  auto emitBox = [&](const int lo, const int hi, const uint16_t lookupOff, const uint16_t lookupLen) {
    char buf[kMaxJoinedBytes];
    size_t len = 0;
    WordInfo w = words[lo];
    if (lo == hi) {
      appendText(pool.data() + words[lo].textOffset, words[lo].textLen, w.textOffset);
      w.textLen = words[lo].textLen;
    } else {
      joinText(words, pool, lo, hi, false, buf, len);  // fits: the whole group's lookup did
      appendText(buf, len, w.textOffset);
      w.textLen = static_cast<uint16_t>(len);
    }
    w.lookupOffset = lookupOff;
    w.lookupLen = lookupLen;
    const WordInfo& l = words[hi];
    if (vertical) {
      w.width = static_cast<int16_t>(l.screenY + l.width - w.screenY);
    } else {
      w.width = static_cast<int16_t>(l.screenX + l.width - w.screenX);
    }
    w.continuationIndex = -1;
    w.continuationOf = -1;
    outWords.push_back(w);
    outSpans.push_back({static_cast<int16_t>(lo), static_cast<int16_t>(hi)});
  };

  for (int t = 0; t < n && !failed;) {
    const int last = groupEnd[t];
    if (last < 0) {
      // Untouched token: its own text, and its own lookup when that differs (a hyphen pair).
      WordInfo w = words[t];
      appendText(pool.data() + words[t].textOffset, words[t].textLen, w.textOffset);
      if (words[t].lookupOffset == words[t].textOffset && words[t].lookupLen == words[t].textLen) {
        w.lookupOffset = w.textOffset;
      } else {
        appendText(pool.data() + words[t].lookupOffset, words[t].lookupLen, w.lookupOffset);
      }
      origToOut[t] = static_cast<int16_t>(outWords.size());
      outWords.push_back(w);
      outSpans.push_back({static_cast<int16_t>(t), static_cast<int16_t>(t)});
      t++;
      continue;
    }
    char buf[kMaxJoinedBytes];
    size_t len = 0;
    if (!joinText(words, pool, t, last, true, buf, len)) {
      // Too long to carry as one token: leave the group's tokens single.
      groupEnd[t] = -1;
      continue;
    }
    // The tokens may run past the word at either end: an opening mark before it (“出), glued
    // punctuation after it (础”). The lookup is the word alone.
    if (wordLead[t] > 0 || static_cast<size_t>(glyphs[last].start) + glyphs[last].count >
                               static_cast<size_t>(glyphs[t].start) + wordLead[t] + wordCells[t]) {
      sliceCodepoints(buf, len, wordLead[t], wordCells[t]);
    }
    uint16_t lookupOff = 0;
    appendText(buf, len, lookupOff);
    int split = last + 1;
    for (int k = t + 1; k <= last; k++) {
      if (words[k].row != words[k - 1].row) {
        split = k;
        break;
      }
    }
    const int firstOut = static_cast<int>(outWords.size());
    emitBox(t, split - 1, lookupOff, static_cast<uint16_t>(len));
    if (split <= last) {
      emitBox(split, last, lookupOff, static_cast<uint16_t>(len));
      outWords[firstOut].continuationIndex = static_cast<int16_t>(firstOut + 1);
      outWords[firstOut + 1].continuationOf = static_cast<int16_t>(firstOut);
    }
    t = last + 1;
  }
  if (failed) {
    outWords.clear();
    outRows.clear();
    outPool.clear();
    outSpans.clear();
    return false;
  }

  for (size_t i = 0; i < outWords.size(); i++) {
    // An untouched hyphen pair still points at original indices; a merged pair already points
    // at output ones.
    if (origToOut[outSpans[i].lo] != static_cast<int16_t>(i)) continue;
    WordInfo& w = outWords[i];
    if (w.continuationIndex >= 0) w.continuationIndex = w.continuationIndex < n ? origToOut[w.continuationIndex] : -1;
    if (w.continuationOf >= 0) w.continuationOf = w.continuationOf < n ? origToOut[w.continuationOf] : -1;
  }

  // Same rows, same order: every row keeps at least one token, since a merged word leaves one
  // token on each row it touches.
  outRows = rows;
  for (auto& r : outRows) r.wordCount = 0;
  for (size_t i = 0; i < outWords.size(); i++) {
    const int r = outWords[i].row;
    if (r < 0 || r >= static_cast<int>(outRows.size())) continue;
    if (outRows[r].wordCount == 0) outRows[r].firstWord = static_cast<int16_t>(i);
    outRows[r].wordCount++;
  }
  return true;
}

int mergedIndexOf(const std::vector<Span>& spans, const int orig) {
  // Spans are ordered and contiguous, so a binary search on lo finds the holder.
  auto it = std::upper_bound(spans.begin(), spans.end(), orig, [](const int v, const Span& s) { return v < s.lo; });
  if (it == spans.begin()) return -1;
  --it;
  if (orig > it->hi) return -1;
  return static_cast<int>(it - spans.begin());
}

}  // namespace CjkWordMerge
