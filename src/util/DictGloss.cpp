#include "DictGloss.h"

#include <algorithm>
#include <cstring>

namespace DictGloss {

namespace {

// Longest bracketed run still treated as a reading. Pinyin for the longest word anyone glosses
// runs to about seven syllables — "zhong1 hua2 ren2 min2 gong4 he2 guo2", 35 bytes with the
// brackets. Past 64 the run is a bracketed something else, so it is left unbolded rather than
// bolding most of a row.
constexpr size_t kMaxReadingBytes = 64;

// How far into the entry the reading's '[' may start. It follows the headword, which is a handful
// of Han characters (3 bytes each) — 96 covers a long one with room to spare, and stops a bracket
// deep inside the definition text from being mistaken for a reading.
constexpr size_t kMaxReadingOffset = 96;

// Three ASCII dots, not U+2026. No font on the card is guaranteed to carry General
// Punctuation, and a missing glyph draws as U+FFFD, which EpdFont renders as a bare '?' — the
// same silent substitution that once put a '?' in front of every dictionary cross-reference.
constexpr char kEllipsis[] = "...";
constexpr size_t kEllipsisLen = 3;

// Drop a trailing incomplete UTF-8 sequence from [s, s+len). Only bytes that were actually
// filled are inspected, so this is safe on a buffer whose tail is uninitialised.
size_t trimTrailingPartialUtf8(const char* s, size_t len) {
  size_t lead = len;
  int back = 0;
  while (lead > 0 && back < 4 && (static_cast<unsigned char>(s[lead - 1]) & 0xC0) == 0x80) {
    lead--;
    back++;
  }
  if (lead == 0) return len;  // nothing but continuation bytes: not a truncation we can fix
  const auto b = static_cast<unsigned char>(s[lead - 1]);
  size_t need = 1;
  if ((b & 0xF8) == 0xF0) {
    need = 4;
  } else if ((b & 0xF0) == 0xE0) {
    need = 3;
  } else if ((b & 0xE0) == 0xC0) {
    need = 2;
  }
  const size_t have = len - (lead - 1);
  return have < need ? lead - 1 : len;
}

// Largest n <= max that does not split a UTF-8 sequence in s.
size_t fitToCodepoint(const char* s, size_t max) {
  size_t n = max;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
  return n;
}

// Append the ellipsis to a NUL-terminated row, dropping trailing codepoints until it fits.
void appendEllipsis(char* row) {
  // Makes the loop's non-underflow structural rather than incidental: entering it requires
  // len > kRowBytes - (kEllipsisLen + 1), which with a row this size is always >= 1.
  static_assert(GlossResult::kRowBytes > kEllipsisLen + 1, "row must have room for an ellipsis");
  size_t len = strlen(row);
  while (len + kEllipsisLen + 1 > GlossResult::kRowBytes) {
    len = fitToCodepoint(row, len - 1);
  }
  memcpy(row + len, kEllipsis, kEllipsisLen + 1);
}

// Copy `src` into `dst` as ONE row of at most maxWidth pixels, ellipsising it if it does not fit.
// Returns the byte length written.
//
// The ellipsis is applied here rather than by setting GlossResult::truncated, so that the tail
// pass at the end of fit() — which appends to the LAST row — cannot append a second one to a row
// that already carries it. That collision is reachable: an entry can consist of a reading and
// nothing else, leaving the reading as both the first and the last row.
size_t fitSingleRow(char* dst, const char* src, const int maxWidth, const DictLayout::Measurer& measure,
                    const EpdFontFamily::Style style) {
  size_t len = strlen(src);
  if (len > GlossResult::kRowBytes - 1) len = fitToCodepoint(src, GlossResult::kRowBytes - 1);
  memcpy(dst, src, len);
  dst[len] = '\0';
  if (len == 0 || measure(dst, style, false) <= maxWidth) return len;

  // Shorten a codepoint at a time until the row plus its ellipsis fits. Measuring each step
  // rather than estimating: this runs once per cursor move on a string of a few dozen bytes,
  // against an advance table that is already resident.
  while (len > 0) {
    len = fitToCodepoint(dst, len - 1);
    memcpy(dst + len, kEllipsis, kEllipsisLen + 1);
    if (measure(dst, style, false) <= maxWidth) return len + kEllipsisLen;
    dst[len] = '\0';
  }
  return 0;
}

// Sink for DictLayout::Wrapper: copy the first kMaxRows lines into the result and count the
// rest, so `truncated` is accurate rather than a guess. The Wrapper merges same-style runs, so
// a plain-text gloss line is normally a single segment; concatenating them adds no separator
// because the Wrapper already appended its own spaces as text.
void collectRow(void* ctx, DictLayout::LayoutLine&& line) {
  auto* out = static_cast<GlossResult*>(ctx);
  if (out->rowCount >= GlossResult::kMaxRows) {
    out->truncated = true;
    return;
  }
  char* dst = out->rows[out->rowCount];
  size_t used = 0;
  size_t boldStart = 0;
  size_t boldLen = 0;
  for (const auto& seg : line.segments) {
    const bool isBold = seg.style == EpdFontFamily::BOLD;
    size_t n = seg.text.size();
    bool cut = false;
    if (used + n + 1 > GlossResult::kRowBytes) {
      n = fitToCodepoint(seg.text.data(), GlossResult::kRowBytes - 1 - used);
      cut = true;
    }
    memcpy(dst + used, seg.text.data(), n);
    // The FIRST bold segment is the whole of the run: the entry carries a single bold span, and
    // the Wrapper merges adjacent same-style segments, so two disjoint bold runs cannot reach one
    // row. Taking only the first keeps that assumption from silently becoming a wrong offset.
    if (isBold && boldLen == 0) {
      boldStart = used;
      boldLen = n;
    }
    used += n;
    if (cut) {
      out->truncated = true;
      break;
    }
  }
  dst[used] = '\0';
  out->boldStart[out->rowCount] = static_cast<uint8_t>(boldStart);
  out->boldLen[out->rowCount] = static_cast<uint8_t>(boldLen);
  out->rowCount++;
}

}  // namespace

ReadingSpan findReading(const char* s) {
  ReadingSpan out;
  if (s == nullptr) return out;
  for (size_t i = 0; i <= kMaxReadingOffset && s[i] != '\0'; i++) {
    if (s[i] == '\n') break;  // first line only
    if (s[i] != '[') continue;
    for (size_t j = i + 1; j - i <= kMaxReadingBytes && s[j] != '\0'; j++) {
      // A newline or a second '[' before the closer means this bracket is not a reading. Stop
      // rather than pairing it with some later ']' and bolding everything in between.
      if (s[j] == '\n' || s[j] == '[') return out;
      if (s[j] != ']') continue;
      if (j == i + 1) return out;  // "[]" — nothing to bold
      out.start = i;
      out.len = j - i + 1;
      out.found = true;
      return out;
    }
    // The first '[' had no usable closer. Whatever it is, it is not the reading, and the reading
    // is defined as the first bracketed run — so there is nothing further to look for.
    return out;
  }
  return out;
}

size_t readEntry(Dictionary::LookupCtx& ctx, HalFile& dictFile, const char* token, char* buf, size_t bufSize) {
  if (token == nullptr || *token == '\0' || buf == nullptr || bufSize < 2) return 0;
  if (!dictFile.isOpen()) return 0;

  const DictLocation loc = Dictionary::locateIn(ctx, token);
  if (!loc.found || loc.size == 0) return 0;
  if (!dictFile.seekSet(loc.offset)) return 0;

  const size_t want = std::min(static_cast<size_t>(loc.size), bufSize - 1);
  const int n = dictFile.read(reinterpret_cast<uint8_t*>(buf), static_cast<int>(want));
  if (n <= 0) return 0;

  size_t len = static_cast<size_t>(n);
  if (len < loc.size) len = trimTrailingPartialUtf8(buf, len);
  buf[len] = '\0';
  return len;
}

void fit(char* buf, const DictLayout::WrapMetrics& metrics, const DictLayout::Measurer& measure, GlossResult& out,
         const FitOptions opts) {
  out.reset();
  if (buf == nullptr || buf[0] == '\0') return;

  // Asked before the sanitiser turns newlines into spaces, when "the first line" still exists as
  // something findReading can see. Only the answer is kept, not the offsets: collapsing whitespace
  // moves them. It cannot change WHICH bracket is first, though — the sanitiser neither adds nor
  // removes brackets, nor reorders anything — so re-asking afterwards yields the same run.
  const bool readingOnFirstLine = findReading(buf).found;

  // Sanitise in place. Every control byte becomes a space, whitespace runs collapse to one,
  // and leading whitespace goes: the Wrapper's fast path (whole span fits on one line) appends
  // a span verbatim, so a leading space would indent the first row and a run of them would
  // spend visible width on nothing. Newlines are separators rather than row breaks on purpose
  // — a 'm' entry uses them between senses, and with only three rows visible a flowing block
  // shows far more than one sense per row.
  char* dst = buf;
  bool pendingSpace = false;
  for (const char* src = buf; *src != '\0'; src++) {
    const auto c = static_cast<unsigned char>(*src);
    if (c <= 0x20 || c == 0x7F) {
      pendingSpace = dst != buf;  // never leading
      continue;
    }
    if (pendingSpace) {
      *dst++ = ' ';
      pendingSpace = false;
    }
    *dst++ = *src;
  }
  *dst = '\0';
  if (buf[0] == '\0') return;

  DictLayout::LineSink sink{&out, &collectRow};
  DictLayout::Wrapper wrapper(metrics, measure, sink);

  // Headword / reading / definition as three spans rather than one, so the Wrapper carries the
  // style through its own segments and the row-to-byte mapping comes back exact — including when
  // the reading straddles a line break, where deriving offsets afterwards would have to account
  // for the spaces the Wrapper dropped to break on. Each split is a terminator swapped in and out
  // again, never a copy of the entry.
  const ReadingSpan reading = readingOnFirstLine ? findReading(buf) : ReadingSpan{};
  const size_t readingEnd = reading.start + reading.len;
  const bool columnar = reading.found && opts.readingOnOwnRow;

  // Columnar layout: the reading owns row 0, written straight into the row rather than fed through
  // the Wrapper — the Wrapper would break an overlong reading across lines and eat the body's
  // rows, where here it is capped at one row by construction. Everything after it wraps through
  // the same Wrapper as always, into whatever rows are left.
  if (columnar) {
    const char saved = buf[readingEnd];
    buf[readingEnd] = '\0';
    const size_t len = fitSingleRow(out.rows[0], buf + reading.start, metrics.maxWidth, measure, EpdFontFamily::BOLD);
    buf[readingEnd] = saved;

    out.boldStart[0] = 0;
    out.boldLen[0] = static_cast<uint8_t>(len);
    out.rowCount = 1;
  }

  if (reading.found) {
    // The leading field — the script variant — is dropped when the caller is drawing it as its own
    // column. Kept otherwise, including in the columnar layout when the box was too narrow for a
    // second cell: there it flows ahead of the definition rather than being lost, and row 0 stays
    // pure reading either way.
    if (reading.start > 0 && !opts.dropLeadingField) {
      const char saved = buf[reading.start];
      buf[reading.start] = '\0';
      StyledSpan head;
      head.text = buf;
      wrapper.onSpan(head);
      buf[reading.start] = saved;
    }

    if (!columnar) {
      const char saved = buf[readingEnd];
      buf[readingEnd] = '\0';
      StyledSpan bold;
      bold.text = buf + reading.start;
      bold.bold = true;
      wrapper.onSpan(bold);
      buf[readingEnd] = saved;
    }

    StyledSpan body;
    body.text = buf + readingEnd;
    wrapper.onSpan(body);
  } else {
    StyledSpan span;
    span.text = buf;
    wrapper.onSpan(span);
  }
  wrapper.finish();

  if (out.truncated && out.rowCount > 0) {
    const int last = out.rowCount - 1;
    appendEllipsis(out.rows[last]);
    // The ellipsis can shorten the row it lands on. Left unclamped, the bold range would point
    // past the terminator and the draw would split the row outside its own string.
    const size_t len = strlen(out.rows[last]);
    if (out.boldStart[last] >= len) {
      out.boldStart[last] = 0;
      out.boldLen[last] = 0;
    } else if (out.boldStart[last] + out.boldLen[last] > len) {
      out.boldLen[last] = static_cast<uint8_t>(len - out.boldStart[last]);
    }
  }
  out.found = out.rowCount > 0;
}

}  // namespace DictGloss
