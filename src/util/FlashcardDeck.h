#pragma once

#include <cstdint>
#include <string>

// Per-book spaced-repetition flashcard deck. Stored as
// <cachePath>/dictionary_flashcards.txt, one card per line:
//
//     word|box|dueDay|chapter|excerpt
//
// - word    : headword/phrase (the dictionary lookup word).
// - box     : Leitner box 0..5, or 255 (RETIRED) once graduated.
// - dueDay  : days-since-2000 (readingHistoryDayIndex); 0 = new/never scheduled.
// - chapter : TOC chapter title the word was looked up in (pipe-stripped, may be
//             empty). Field 4, never the remainder, so it must not contain '|'.
// - excerpt : page-local sentence the word was looked up from (may be empty).
//             The remainder of the line, so an embedded '|' here is harmless.
//
// Legacy 4-field lines (word|box|dueDay|excerpt, no chapter) parse with an empty
// chapter and the excerpt intact, so an older deck upgrades cleanly.
//
// The card BACK face is never stored: it is the live dictionary definition,
// re-rendered on demand by DictionaryLookupController. Only the front context
// (word + excerpt) and the Leitner schedule live here.
//
// Cards are enrolled at the in-book lookup gesture (the sole site with page
// context for the excerpt) and graded during a review session. The deck is
// LOCAL-ONLY: unlike LookupHistory it is never synced, so there is no Lamport
// clock and no tombstone machinery -- a delete is just a row removal.
//
// Every mutator streams the file line-by-line through a fixed stack buffer and
// never materializes the deck in RAM (the same OOM discipline as LookupHistory:
// this runs while the EPUB reader is resident and the largest free block can be
// a few KB; with -fno-exceptions a failed std::vector/std::string growth there
// aborts the firmware). The read buffer is larger here (512 vs LookupHistory's
// 256) because lines carry an excerpt.
class FlashcardDeck {
 public:
  // Backing-file name inside a book's cache dir.
  static constexpr char FILE_NAME[] = "dictionary_flashcards.txt";

  // Leitner schedule: interval (days) applied when a card is promoted INTO each
  // box. Boxes 0..5; the two 16-day reps at the top are the "2 stable reps"
  // before graduation. constexpr -> flash (CLAUDE.md rule 6), integer-only.
  static constexpr uint16_t BOX_INTERVAL_DAYS[6] = {1, 2, 4, 8, 16, 16};
  static constexpr uint8_t TOP_BOX = 5;
  static constexpr uint8_t RETIRED = 255;  // graduated; never scheduled again.

  // Excerpt is capped so a line fits the 512-byte streaming buffer with room to
  // spare for the word + the box/dueDay/chapter fields.
  static constexpr int EXCERPT_MAX = 160;
  static constexpr int CHAPTER_MAX = 80;

  enum class SessionScope : uint8_t { DueFirst = 0, AllShuffled = 1 };

  struct Entry {
    std::string word;
    uint8_t box = 0;
    uint32_t dueDay = 0;
    std::string chapter;
    std::string excerpt;
  };

  // Deck-wide review stats, computed in one streaming pass (no materialization).
  // `boxHist[b]` counts cards currently in box b (0..5); retired cards are in
  // `mastered`, not the histogram. `due` counts isDue() cards; `nextDueDay` is
  // the soonest future scheduled day (> today, non-retired), 0 if none.
  struct Stats {
    int total = 0;
    int due = 0;
    int mastered = 0;
    int boxHist[6] = {0, 0, 0, 0, 0, 0};
    uint32_t nextDueDay = 0;
  };

  // --- Leitner core (pure, no I/O -- directly unit-testable) ----------------

  // Apply one graded recall in place. Miss -> box 0. Hit at the top box ->
  // RETIRED (graduated). Otherwise promote one box. dueDay is re-scheduled to
  // `today + BOX_INTERVAL_DAYS[box]` (untouched on graduation).
  static void applyGrade(uint8_t& box, uint32_t& dueDay, bool correct, uint32_t today);

  // A card is due if it is not retired and either never scheduled (dueDay==0)
  // or its due day has arrived.
  static bool isDue(uint8_t box, uint32_t dueDay, uint32_t today);
  static bool isMastered(uint8_t box) { return box == RETIRED; }

  // --- Deck store -----------------------------------------------------------

  // Enroll `word` with `excerpt` + `chapter` as a fresh box-0 / dueDay-0 (new)
  // card. If the word is already in the deck its prior row is dropped and
  // re-appended as newest, resetting it to box 0 / new. Each of excerpt and
  // chapter is overwritten only when the new value is non-empty (a re-lookup
  // from the history list passes ""), so a re-enroll without page context keeps
  // the original context. Values are control-char-stripped and capped
  // (EXCERPT_MAX / CHAPTER_MAX); chapter additionally has '|' stripped.
  // Returns false on I/O failure.
  static bool enroll(const std::string& cachePath, const std::string& word, const std::string& excerpt,
                     const std::string& chapter = "");

  // Total card count without materializing the deck (one streaming pass).
  static int count(const std::string& cachePath);

  // Deck-wide review stats in one streaming pass (for the pre-session overview).
  static Stats computeStats(const std::string& cachePath, uint32_t today);

  // Fill out[0..n) with up to `n` cards newest-first starting at newest-first
  // index `startNewest` (0 = most recently enrolled). One streaming pass.
  // Returns the number actually filled.
  static int loadWindow(const std::string& cachePath, int startNewest, int n, Entry* out);

  // Remove the card at 0-based file index (oldest=0). Local-only, no tombstone.
  static bool removeAt(const std::string& cachePath, int index);

  // Grade the card for `word`: load its box/dueDay, apply applyGrade(), rewrite
  // the row in place (excerpt preserved, order unchanged). No-op if the word is
  // absent. Returns false on I/O failure.
  static bool grade(const std::string& cachePath, const std::string& word, bool correct, uint32_t today);

  // Select up to `cap` cards for a review session, writing their newest-first
  // indices into out[0..cap). Returns the count selected. Pure SELECTION (no
  // shuffle): the caller shuffles the returned window for presentation -- this
  // keeps selection deterministic and host-testable.
  //   DueFirst (today>0): scheduled-and-due cards newest-first, then new
  //     (never-scheduled) cards fill any remaining budget.
  //   AllShuffled (or today==0, clock unavailable): any non-retired cards,
  //     newest-first, up to cap.
  static int buildSession(const std::string& cachePath, SessionScope scope, uint32_t today, int cap, uint16_t* out);

  // Stream each line of `path` through `fn` (fixed 512-byte line buffer, no
  // heap). `fn` returns false to stop early. Returns false iff the file could
  // not be opened. Public so the buildSession selection helpers (and tests) can
  // reuse the streaming primitive.
  static bool forEachLine(const std::string& path, bool (*fn)(void* ctx, const char* line, int len), void* ctx);

 private:
  static std::string filePath(const std::string& cachePath);
  static std::string tmpFilePath(const std::string& cachePath);
};
