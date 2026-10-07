#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "PageTokenScan.h"

/**
 * Locating a stored quote's text among the words of a rendered page.
 *
 * A mark made on a KOReader peer carries no word range -- page-local indices describe
 * only the layout that produced them -- so the range is re-derived at draw time by
 * finding the quoted text in the page's own token sequence. Pure and header-only so the
 * rule can be tested against PageTokens directly; PageMarks owns the page walk.
 */
namespace SnippetMatch {

/**
 * Length of `text` with a trailing hyphen -- real or soft -- removed.
 *
 * A snippet stores a hyphenated word merged, while the page splits it across two tokens.
 */
inline size_t withoutTrailingHyphen(const char* text, size_t len) {
  if (len == 0) return 0;
  if (text[len - 1] == '-') return len - 1;
  if (len >= 2 && static_cast<uint8_t>(text[len - 2]) == 0xC2 && static_cast<uint8_t>(text[len - 1]) == 0xAD) {
    return len - 2;
  }
  return len;
}

/**
 * One unit of text for matching: a byte, or a typographic quote folded to its ASCII form, with
 * its byte length in `n`.
 *
 * Copies of one book routinely differ only in quote style -- a re-converted EPUB, or a mark made
 * on another device's copy -- and a mark saved against one must still find its words in the other.
 */
inline char foldedUnit(const char* s, const size_t avail, size_t& n) {
  if (avail >= 3 && static_cast<uint8_t>(s[0]) == 0xE2 && static_cast<uint8_t>(s[1]) == 0x80) {
    const uint8_t c = static_cast<uint8_t>(s[2]);
    if (c >= 0x98 && c <= 0x9B) {  // ‘ ’ ‚ ‛
      n = 3;
      return '\'';
    }
    if (c >= 0x9C && c <= 0x9F) {  // “ ” „ ‟
      n = 3;
      return '"';
    }
  }
  n = 1;
  return s[0];
}

/**
 * How many bytes of `a` the whole of `b` covers, comparing under foldedUnit; -1 when they
 * differ, or when `a` runs out first -- `aRanOut` is then set if everything up to there agreed.
 */
inline int foldedPrefix(const char* a, const size_t alen, const char* b, const size_t blen, bool& aRanOut) {
  aRanOut = false;
  size_t i = 0;
  size_t j = 0;
  while (j < blen) {
    if (i >= alen) {
      aRanOut = true;
      return -1;
    }
    size_t na = 0;
    size_t nb = 0;
    if (foldedUnit(a + i, alen - i, na) != foldedUnit(b + j, blen - j, nb)) return -1;
    i += na;
    j += nb;
  }
  return static_cast<int>(i);
}

/** One quote being matched against the page, a token at a time. */
struct Matcher {
  const char* snippet = nullptr;
  // Offsets into the snippet, which is capped at the caller's buffer size, so a byte each.
  uint8_t partStart = 0;   // the part being matched
  uint8_t partLen = 0;     // its length; 0 once the snippet is spent
  uint8_t cursor = 0;      // where the scan for the part after it resumes
  uint8_t matchedLen = 0;  // how much of it the tokens so far have covered
  uint16_t start = 0;
  bool open = false;
  bool done = false;
  bool lastPart = false;   // no further part follows the one being matched
  bool truncated = false;  // the snippet fills its buffer, so its tail may be a part word

  // PageTokens::nextTextPart applies the page's own tokenisation to the snippet, so the
  // two sequences line up: a word with no letter or digit is skipped on both sides, and
  // a dash separates parts on both. Matching on raw spaces instead would stall on the
  // first standalone dash, which the page never hands over as a token.
  void takePart(size_t from) {
    size_t s0 = 0;
    size_t len = 0;
    size_t next = 0;
    if (!PageTokens::nextTextPart(snippet, from, s0, len, next)) {
      partLen = 0;
      return;
    }
    partStart = static_cast<uint8_t>(s0);
    partLen = static_cast<uint8_t>(len);
    cursor = static_cast<uint8_t>(next);
    matchedLen = 0;
    size_t s1 = 0;
    size_t l1 = 0;
    size_t n1 = 0;
    lastPart = !PageTokens::nextTextPart(snippet, next, s1, l1, n1);
  }

  void reset() {
    open = false;
    takePart(0);
  }

  /**
   * Start matching `snippet` against a page.
   *
   * @param cap the snippet buffer's size, NUL included. Whether the tail may be a part
   *            word is a property of the snippet, not of the attempt, and reset() runs
   *            again on every token that breaks a run -- so it is settled once, here.
   */
  void begin(const size_t cap) {
    truncated = cap > 0 && strnlen(snippet, cap) >= cap - 1;
    reset();
  }

  /** Offer one page token. Returns true once the whole snippet has been matched. */
  bool offer(const char* text, size_t len, const uint16_t index) {
    if (done || partLen == 0) return false;
    len = withoutTrailingHyphen(text, len);
    if (len == 0) return false;

    const size_t remaining = partLen - matchedLen;
    bool snippetRanOut = false;
    const int covered = foldedPrefix(snippet + partStart + matchedLen, remaining, text, len, snippetRanOut);
    // A snippet is cut to fit its buffer wherever the cut falls, so the last part of a
    // full one is usually the head of a longer word: "...playground, so l" for "let's".
    // Requiring the page's word to fit inside it fails every highlight over the cap.
    // Only the last part, and only when the snippet is full -- a shorter one was not cut,
    // so its tail is a whole word and must match as one.
    if (truncated && lastPart && snippetRanOut && remaining > 0) {
      if (!open) {
        open = true;
        start = index;
      }
      done = true;
      return true;
    }
    if (covered < 0) {
      // Not the occurrence we were following. Start over, and give this same token its
      // chance as a first word rather than skipping it.
      const bool wasOpen = open;
      reset();
      if (!wasOpen || partLen == 0) return false;
      return offer(text, len, index);
    }

    if (!open) {
      open = true;
      start = index;
    }
    matchedLen = static_cast<uint8_t>(matchedLen + covered);
    if (matchedLen < partLen) return false;  // mid-part: a hyphenation break

    takePart(cursor);
    if (partLen == 0) {
      done = true;
      return true;
    }
    return false;
  }
};

}  // namespace SnippetMatch
