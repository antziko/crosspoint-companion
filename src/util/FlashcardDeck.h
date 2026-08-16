#pragma once

#include <cstdint>
#include <string>

class HalFile;  // fwd-decl: rewriteDeck's callbacks take a HalFile& (defined in <HalStorage.h>)

// Per-book spaced-repetition flashcard deck. Stored as
// <cachePath>/dictionary_flashcards.txt, one card per line:
//
//     word|box|dueDay|chapter|version|excerpt
//
// - word    : headword/phrase (the dictionary lookup word).
// - box     : Leitner box 0..5, or 255 (RETIRED) once graduated.
// - dueDay  : days-since-2000 (readingHistoryDayIndex); 0 = new/never scheduled.
// - chapter : TOC chapter title the word was looked up in (pipe-stripped, may be
//             empty). Delimited field, never the remainder, so it must not '|'.
// - version : per-word Lamport version for cross-device sync (0 = legacy/unset).
//             Bumped on enroll; preserved by grade/suspend (schedule is local).
// - excerpt : page-local sentence the word was looked up from (may be empty).
//             The remainder of the line, so an embedded '|' here is harmless.
//
// Legacy lines upgrade cleanly: a 5-field "word|box|dueDay|chapter|excerpt" (no
// version) parses with version 0 (the field between chapter and excerpt is read
// as a version only when it is all-digits), and a 4-field "word|box|dueDay|
// excerpt" (no chapter) parses with empty chapter + version 0.
//
// The card BACK face is never stored: it is the live dictionary definition,
// re-rendered on demand by DictionaryLookupController. Only the front context
// (word + chapter + excerpt) and the Leitner schedule live here.
//
// Cards are enrolled at the in-book lookup gesture (the sole site with page
// context for the excerpt) and graded during a review session.
//
// Cross-device sync: card CONTENT (word + chapter + excerpt) is synced via the
// KOSync stats "fc" blob, Lamport-versioned exactly like LookupHistory's "dh".
// The SRS SCHEDULE (box/dueDay) is never synced -- a received card is box-0/new
// and studied on each device's own clock. Sidecars hold the sync state:
//   dictionary_flashcards.ver   Lamport clock (one int)
//   dictionary_flashcards.tomb  deleted "word|VER" tombstones
//   dictionary_flashcards.sync  "lastVer cursorIndex lastDeviceCount tombCursorIndex" watermark
// The deck file stays the single source of per-word versions (inline, above).
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
  static constexpr uint8_t RETIRED = 255;    // graduated; never scheduled again.
  static constexpr uint8_t SUSPENDED = 254;  // set aside by the user; never
                                             // scheduled until explicitly
                                             // unsuspended (or cleared on PC).

  // Excerpt is capped so a line fits the 512-byte streaming buffer with room to
  // spare for the word + the box/dueDay/chapter fields.
  static constexpr int EXCERPT_MAX = 160;
  static constexpr int CHAPTER_MAX = 80;

  // Cross-device sync ("fc" blob): adaptive per-sync slice cap bounds, in bytes.
  // The blob is bounded by a card *count* (it stops when the next card would
  // overflow the cap), so a large deck propagates over several syncs instead of
  // one huge blob. The floor keeps the upload contig-safe and the GET aggregate
  // small when heap is low / many devices share the GET; the ceiling speeds
  // backfill when heap is high / few devices. See adaptiveSliceCap().
  static constexpr size_t FC_SLICE_FLOOR = 2048;
  static constexpr size_t FC_SLICE_CEIL = 6144;

  // DueFirst/AllShuffled are the two normal review orders. Suspended is a
  // dedicated pass over set-aside cards (box==SUSPENDED) for unsuspending them;
  // it is never persisted as the default scope.
  enum class SessionScope : uint8_t { DueFirst = 0, AllShuffled = 1, Suspended = 2 };

  struct Entry {
    std::string word;
    uint8_t box = 0;
    uint32_t dueDay = 0;
    std::string chapter;
    std::string excerpt;
    uint32_t version = 0;  // per-word Lamport version (sync); 0 for legacy lines
    uint32_t count = 1;    // local lookup count (times re-enrolled); 1 for legacy lines
  };

  // Deck-wide review stats, computed in one streaming pass (no materialization).
  // `boxHist[b]` counts cards currently in box b (0..5); retired cards are in
  // `mastered`, suspended cards in `suspended` -- neither is in the histogram.
  // `due` counts isDue() cards; `nextDueDay` is the soonest future scheduled day
  // (> today, non-retired), 0 if none.
  struct Stats {
    int total = 0;
    int due = 0;
    int mastered = 0;
    int suspended = 0;
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
  static bool isSuspended(uint8_t box) { return box == SUSPENDED; }

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
  // Returns the number actually filled. When `wordsOnly` is true, only
  // word/box/dueDay are populated (chapter/excerpt left empty) -- the list view
  // shows word + box glyph only, so skipping the two excerpt/chapter string
  // assignments per row keeps the window's heap at one allocation per row.
  static int loadWindow(const std::string& cachePath, int startNewest, int n, Entry* out, bool wordsOnly = false);

  // Remove the card at 0-based file index (oldest=0). Drops the row and writes a
  // version-stamped tombstone so the delete propagates on sync.
  static bool removeAt(const std::string& cachePath, int index);

  // Remove the card matching `word` (the by-word analogue of removeAt, matching
  // grade/suspend/unsuspend which all key on the word). Drops the matching row
  // and writes a version-stamped tombstone (delete propagates on sync). No-op if
  // the word is absent. Returns false on I/O failure.
  static bool remove(const std::string& cachePath, const std::string& word);

  // Grade the card for `word`: load its box/dueDay, apply applyGrade(), rewrite
  // the row in place (excerpt preserved, order unchanged). No-op if the word is
  // absent. Returns false on I/O failure.
  static bool grade(const std::string& cachePath, const std::string& word, bool correct, uint32_t today);

  // Set `word` aside: rewrite its row with box=SUSPENDED and dueDay=0 (excerpt,
  // chapter and order preserved). A suspended card is skipped by every normal
  // review session (isDue / buildSession exclude it) until unsuspend(). No-op if
  // the word is absent. Returns false on I/O failure.
  static bool suspend(const std::string& cachePath, const std::string& word);

  // Reverse suspend(): rewrite `word`'s row back to a fresh card (box=0,
  // dueDay=0). The card re-enters the new-card pool -- its pre-suspend box is not
  // recoverable (it was overwritten by SUSPENDED). No-op if the word is absent.
  // Returns false on I/O failure.
  static bool unsuspend(const std::string& cachePath, const std::string& word);

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

  // --- Cross-device sync ("fc" blob; mirrors LookupHistory's "dh") -----------
  //
  // Wire format (one line each, '\n'-terminated):
  //   H<word>|<chapter>|<version>|<excerpt>   a card (content only, no schedule)
  //   T<word>|<version>                       a tombstone (delete)
  // Box/dueDay never go on the wire: a merged card is always box-0 / dueDay-0.

  // Outcome of a serialize pass (for upload bookkeeping + tests).
  struct BlobStats {
    int histCount = 0;            // phase-2 delta cards (new/changed since last upload)
    int rollCount = 0;            // phase-4 rolling-slice cards (re-broadcast heal)
    int tombCount = 0;            // phase-1 NEW tombstones (version > lastVer)
    int tombRollCount = 0;        // phase-3 rolling-slice tombstones (re-broadcast heal)
    uint32_t maxVer = 0;          // highest version emitted (advances the watermark)
    uint32_t nextCursor = 0;      // deck file index to resume the card rolling slice next time
    uint32_t nextTombCursor = 0;  // tomb file index to resume the tomb rolling slice next time
    bool truncated = false;       // a line did not fit the cap (rolling slice stopped)
  };

  // Upload watermark persisted in dictionary_flashcards.sync as four decimals
  // "lastVer cursorIndex lastDeviceCount tombCursorIndex". lastVer = highest version
  // known to be accepted by the server (delta filter). cursorIndex = where the rolling
  // card heal slice resumes. lastDeviceCount = devices seen on the last GET (caps
  // sizing). tombCursorIndex = where the rolling tomb heal slice resumes. The 4th field
  // is optional on read: a legacy 3-field file loads tombCursorIndex = 0 (no version
  // bump needed -- the format is whitespace-delimited and forward/backward tolerant).
  struct SyncWatermark {
    uint32_t lastVer = 0;
    uint32_t cursorIndex = 0;
    uint32_t lastDeviceCount = 0;
    uint32_t tombCursorIndex = 0;
  };

  // Adaptive per-sync slice cap from free heap + the last-seen device count.
  // Pure (no I/O) so it is directly unit-testable. Grows toward FC_SLICE_CEIL
  // when heap is high and few devices share the GET; clamps to FC_SLICE_FLOOR
  // otherwise; never lets deviceCount*(reserve+cap) approach the GET ceiling.
  static size_t adaptiveSliceCap(uint32_t freeHeap, uint32_t deviceCount);

  // Serialize the upload blob into out[0..cap): all tombstones first (delete
  // propagation), then changed cards (version > watermark, newest-first, low
  // latency for new enrollments), then a rolling slice of older cards from the
  // persisted cursor (heals a fresh device over successive syncs). Stops when a
  // card would overflow the cap. Returns bytes written; fills outStats.
  static size_t serializeForUpload(const std::string& cachePath, uint8_t* out, size_t cap, BlobStats* outStats);

  // Advance the watermark to the max version sent, persist the rolling cursor
  // and the device count -- ONLY after a confirmed PUT (a failed PUT re-sends
  // the same slice next time).
  static void commitUpload(const std::string& cachePath, const BlobStats& uploaded, uint32_t deviceCount);

  // Merge a remote "fc" blob, field-level + version-wins. A new word is appended
  // box-0/dueDay-0 with the wire content; an existing word newer on the wire has
  // its chapter/excerpt/version updated in place (box/dueDay/order preserved); a
  // tombstone newer than what we know removes the card + records the delete.
  // Raises the local Lamport clock past every version seen. Returns cards added;
  // *outDeleted (if non-null) gets the deletes applied.
  static int mergeBlob(const std::string& cachePath, const uint8_t* blob, size_t len, int* outDeleted = nullptr);

  // Liveness pump for mergeBlob — same contract as LookupHistory::setMergeProgressHook:
  // called once per blob line with progress in BYTES across both passes (total = 2 * len),
  // must not allocate or touch the deck files, and must rate-limit itself. Null by default.
  static void setMergeProgressHook(void (*fn)(void* ctx, size_t done, size_t total), void* ctx);

  // Read the upload watermark (exposed for the activity + tests).
  static SyncWatermark loadWatermark(const std::string& cachePath);

 private:
  static std::string filePath(const std::string& cachePath);
  static std::string tmpFilePath(const std::string& cachePath);

  // --- Sync sidecar paths + Lamport/tombstone helpers (clone LookupHistory) --
  static std::string verFilePath(const std::string& cachePath);
  static std::string tombFilePath(const std::string& cachePath);
  static std::string syncFilePath(const std::string& cachePath);
  // Lamport clock (dictionary_flashcards.ver). loadCounter reconstructs from the
  // max version across deck + tombstones if the .ver file is missing (legacy).
  static uint32_t loadCounter(const std::string& cachePath);
  static uint32_t nextVersion(const std::string& cachePath);
  static void observeVersion(const std::string& cachePath, uint32_t v);
  static void storeWatermark(const std::string& cachePath, const SyncWatermark& wm);
  // Word's inline deck version, or -1 if the word is not in the deck.
  static int cardVersionOf(const std::string& cachePath, const std::string& word);
  // Word's tombstone version, or 0 if not tombstoned.
  static uint32_t tombstoneVersionOf(const std::string& cachePath, const std::string& word);
  static void setTombstone(const std::string& cachePath, const std::string& word, uint32_t version);
  static void clearTombstone(const std::string& cachePath, const std::string& word);
  // Merge primitives (no version bump -- the wire version is authoritative):
  // append a brand-new card, update an existing card's content in place, or drop
  // a card row (tombstone is written separately by mergeBlob).
  static bool appendRemoteCard(const std::string& cachePath, const std::string& word, const char* chapter,
                               int chapterLen, const char* excerpt, int excerptLen, uint32_t version);
  static bool updateRemoteCard(const std::string& cachePath, const std::string& word, const char* chapter,
                               int chapterLen, const char* excerpt, int excerptLen, uint32_t version);
  static void removeCardRow(const std::string& cachePath, const std::string& word);
  // Shared fixed-value row rewrite backing suspend()/unsuspend(): force `word`'s
  // box/dueDay, copying all other rows verbatim. No-op if the word is absent.
  static bool setBoxForWord(const std::string& cachePath, const std::string& word, uint8_t box, uint32_t dueDay);

  // Atomic streaming deck rewrite shared by enroll(dedup)/removeAt/grade/setBox.
  // Opens a temp file, streams every existing line through `lineFn` (which writes
  // its output to `out` -- write nothing to drop the row, return false on I/O
  // failure to abort), optionally appends rows via `tailFn`, then replaces the
  // original only after a clean write so a mid-write failure cannot lose the
  // deck. Callers must pre-verify the work is needed (the file exists / the
  // target row is present). Returns false on any I/O failure.
  static bool rewriteDeck(const std::string& cachePath, void* ctx,
                          bool (*lineFn)(void* ctx, HalFile& out, const char* line, int len),
                          bool (*tailFn)(void* ctx, HalFile& out) = nullptr);
};
