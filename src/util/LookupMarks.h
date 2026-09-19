#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Resident index of where this book's looked-up words sit, so the reader can underline them
// on the page they were looked up on.
//
// The anchor comes free from the flashcard deck: every lookup auto-enrolls a card, and the
// card's chapter field carries the in-chapter page as a trailing " X/Y" token (appended in
// EpubReaderActivity::openWordSelect). One streaming pass over the deck at book open turns
// that into the fixed-size table below; the render path then only ever scans RAM.
//
// Words are held as hashes, not text: the table stays a single fixed allocation with no string
// pool behind it, which is what makes it affordable in the reader, where the largest free block
// can be a few KB. A hash collision would underline a word that was not looked up — harmless,
// and at 32 bits over ~128 entries it is not going to happen.
//
// No HalStorage here: the SD pass lives in FlashcardDeck (FlashcardDeck::forEachCardAnchor)
// and feeds this table through add(), which keeps this file host-testable.
class LookupMarks {
 public:
  // One looked-up word, anchored to the page it was looked up on.
  struct Mark {
    uint32_t chapterHash;  // hash of the card's chapter title (the bytes before " X/Y")
    uint32_t wordHash;     // hash of the whole normalised word
    // Hash of the word's FIRST codepoint. CJK is laid out one token per character, so a CJK
    // word can only be matched by opening a run on its first character and accumulating from
    // there; for a word that is one page token this is unused.
    uint32_t headHash;
    uint16_t page;       // 1-based in-chapter page from the card's " X/Y"
    uint16_t pageCount;  // its Y: a chapter that has re-paginated no longer matches
    uint16_t byteLen;    // normalised byte length — where a CJK run stops accumulating
  };

  // Newest N lookups of the book. 128 * 16 B = 2 KB, allocated once per book. Older marks are
  // dropped rather than grown into: the reader's heap is the constraint, and the words you
  // looked up recently are the ones you are still learning.
  static constexpr int MAX_MARKS = 128;

  static LookupMarks& getInstance() {
    static LookupMarks instance;
    return instance;
  }

  // Drop the table and free it. Called when the reader closes the book.
  void clear();

  // Record one card. Allocates the table on first use; a false return means OOM (the caller
  // should stop feeding) or a card with no usable page anchor. Once full, the oldest mark is
  // overwritten.
  bool add(const char* word, int wordLen, const char* chapterTitle, int titleLen, int page, int pageCount);

  int size() const { return count_; }
  bool empty() const { return count_ == 0; }

  // Marks anchored on this page, written to out[0..cap). Returns how many. A pageCount that
  // does not match the chapter's current pagination returns none — the stored page numbers
  // refer to a layout that no longer exists, and marking the wrong word is worse than none.
  int collectForPage(uint32_t chapterHash, int page, int pageCount, const Mark** out, int cap) const;

  // --- Run matching ------------------------------------------------------------------------
  //
  // Whether a page token belongs to a mark. Non-CJK words are one token and settle in a single
  // comparison; CJK lays out one token per CHARACTER, so a marked CJK word is a RUN — opened on
  // a token whose hash matches the word's first character, then accumulating each following
  // token into the same FNV until the word's normalised byte length is reached, marking only if
  // the accumulated hash matches the whole word. One RunState per mark on the page, stepped once
  // per token in page order.
  //
  // A single forward pass with no backtracking: a word immediately preceded by its own first
  // character (中中国人) opens on the wrong character, accumulates to the wrong length and drops
  // the mark. That is the failure mode of a hash-only matcher with no text to re-scan, and it
  // fails toward marking nothing.
  //
  // Deliberately free of geometry, GfxRenderer and Page: callers keep their own spans, and this
  // stays host-testable. It is the ONE place the predicate is written, so the underline
  // PageMarks::drawForPage paints and the mark PageMarks::lookupMarkAtPoint reports can never
  // name different words.
  struct RunState {
    uint32_t hash = 0;
    uint16_t len = 0;
    int16_t y = 0;
    bool open = false;
  };

  // None          nothing to do for this mark on this token.
  // Opened        a CJK run started here; the caller seeds its span from this token.
  // Extended      the open run took this token too; the caller widens its span.
  // MatchedRun    the mark completed on a token that EXTENDED an already-seeded run; the caller
  //               widens the span one last time and marks x0..x1.
  // MatchedToken  the whole word is this single token, so the span is its own box and no run
  //               state is involved. Covers both a non-CJK word and a one-character CJK one --
  //               the case that makes the run/token distinction load-bearing rather than
  //               cosmetic, since a one-token CJK word opens and completes on the same token.
  enum class Step : uint8_t { None, Opened, Extended, MatchedRun, MatchedToken };

  // `tokenHash`/`tokenLen` are hashAppend(FNV_OFFSET, text, len) over this token, which the
  // caller computes once and reuses across every mark on the page. `text`/`len` are the same
  // bytes, needed only to extend an open run. `rowY` abandons a run that wrapped onto a new
  // line — a word split across two lines is not marked rather than marked wrongly.
  static Step step(const Mark& m, RunState& r, bool isCjk, uint32_t tokenHash, uint16_t tokenLen, const char* text,
                   size_t len, int16_t rowY);

  // --- Hashing ---------------------------------------------------------------------------
  //
  // FNV-1a over normalised bytes: ASCII letters folded to lower case, ASCII punctuation and
  // spaces skipped, everything else hashed verbatim. So "Fork," and "fork" agree, and so do
  // "didn't" and "didnt" — the page carries the token as it was typeset, while the card holds
  // whatever cleanWord() left of it.
  static constexpr uint32_t FNV_OFFSET = 2166136261u;

  // Fold `len` bytes of `text` into `h`, adding the number of bytes that survived
  // normalisation to *inOutLen (may be null). This is the primitive the CJK run matcher
  // accumulates through, so it must stay the only place the normalisation rule is written.
  static uint32_t hashAppend(uint32_t h, const char* text, size_t len, uint16_t* inOutLen = nullptr);

  static uint32_t hashWord(const char* text, size_t len) { return hashAppend(FNV_OFFSET, text, len); }

  // Chapter titles are compared whole, so they take the same normalisation.
  static uint32_t hashChapter(const char* title, size_t len) { return hashWord(title, len); }

 private:
  LookupMarks() = default;

  std::unique_ptr<Mark[]> marks_;
  int count_ = 0;     // marks held, <= MAX_MARKS
  int writeIdx_ = 0;  // ring cursor: once full, the oldest mark is the one overwritten
};
