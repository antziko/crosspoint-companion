#pragma once

#include <cstdint>
#include <string>

// Dictionary notes ("highlights"): text kept from a definition, not from a book's own pages.
//
// Scoped to the DICTIONARY, not to the book that was open at the time -- a note is about a
// dictionary entry, so it marks that entry in every book. Stored one file per dictionary as
// /.crosspoint/dictnotes/<dictHash>.txt, one note per line:
//
//     word|dictHash|version|text
//
// - word     : the headword the note was taken from. The caller passes the word ON SCREEN
//              (currentWord_), not the word the definition activity was opened for -- the two
//              diverge on the first chain-forward.
// - dictHash : DictionaryRegistry::nameHash() of the dictionary the text came from, via
//              DictUtils::dictHashOfPath. Also names the file, so it is redundant on the line --
//              kept because it makes a file self-describing if one is ever copied between cards,
//              and it costs nothing.
// - version  : per-note Lamport version, written 0 today. The slot cross-device sync needs;
//              adding it now is what keeps that a code change rather than a format change
//              (dictionary_flashcards.txt carries the same field for the same reason).
// - text     : the selected text. The REMAINDER of the line, so an embedded '|' is harmless;
//              newlines are stripped and the text is capped at TEXT_MAX on a UTF-8 boundary.
//
// A quote on a book page anchors by spine index + page-local word indices (PageMarks.h); a
// definition has no such anchor, so a note stores its text and is matched back by text.
//
// Why /.crosspoint/dictnotes/ and not the book cache: every cache-wiping path filters on
// isBookCacheDirectoryName (BookCacheUtils.cpp:207-217) -- the orphan sweep and all three Clear
// Cache modes -- so a sibling directory survives Delete Cache, Repaginate and Rebuild Covers.
// And not <dictRoot>/<name>/: DictPrepareTask rewrites that folder, and re-preparing a
// dictionary must not take the notes with it. One file PER dictionary rather than one global
// file because loadMarksForWord streams the whole file on every definition open, so the file
// that is read has to stay small.
//
// Every mutator streams the file line-by-line through a fixed stack buffer and never
// materializes the notes in RAM -- the same discipline as LookupHistory and FlashcardDeck: this
// runs while the EPUB reader is resident and the largest free block can be a few KB, and with
// -fno-exceptions a failed std::vector/std::string growth there aborts the firmware.
class DictNotes {
 public:
  // Directory holding one <dictHash>.txt per dictionary. Created on first write.
  static constexpr char NOTES_DIR[] = "/.crosspoint/dictnotes";

  // On-disk text cap. Sized so word + dictHash + version + text fit the 512-byte streaming
  // line buffer with room to spare.
  static constexpr size_t TEXT_MAX = 240;
  // Notes per dictionary. Oldest is evicted, matching how the lookup history caps.
  static constexpr int MAX_NOTES = 200;
  // Notes considered when marking one definition, and the text each keeps for matching.
  static constexpr int MAX_MARKS = 8;
  static constexpr size_t MARK_TEXT_MAX = 160;
  // Dictionaries the picker can list. A card with more than this many dictionaries carrying
  // notes lists the first MAX_DICTS of them.
  static constexpr int MAX_DICTS = 24;

  struct Note {
    std::string word;
    std::string text;
    uint32_t dictHash = 0;
  };

  // One dictionary that has notes, for the screen that lets the reader pick between them.
  // A dictionary is identified by hash alone here: resolving it to a name is the caller's job
  // (DictionaryRegistry::indexOfHash), and an uninstalled dictionary still has to be listed so
  // its notes stay reachable.
  struct DictSummary {
    uint32_t dictHash = 0;
    uint16_t count = 0;
  };

  // One note's text, fixed-size so the marking pass allocates nothing per note.
  struct Mark {
    char text[MARK_TEXT_MAX + 1] = {};
    uint16_t len = 0;
  };

  // Append a note. Returns false on I/O failure, empty text, or dictHash 0 (no dictionary).
  // An exact duplicate (same word and text) is a no-op and returns true: re-highlighting the
  // same sentence should not fill the list with copies.
  static bool add(const std::string& word, uint32_t dictHash, const std::string& text);

  // Total note count for one dictionary, without materializing the file (one streaming pass).
  static int count(uint32_t dictHash);

  // Fill out[0..n) with up to n notes in newest-first order starting at newest-first index
  // `startNewest`. Returns how many were filled. Lets the list screen hold one window.
  static int loadWindow(uint32_t dictHash, int startNewest, int n, Note* out);

  // Remove the note at a 0-based newest-first index. Streaming rewrite through a temp file:
  // the original is replaced only after a clean write.
  static bool removeAt(uint32_t dictHash, int indexNewestFirst);

  // Notes for one headword, newest first, for the in-definition marking pass. Returns how many
  // were written (0 when this word has none, the common case).
  static int loadMarksForWord(const std::string& word, uint32_t dictHash, Mark* out, int cap);

  // Every dictionary that has at least one note, written to out[0..cap). Returns how many.
  // Costs one directory listing plus a streaming count per file, so it is a screen-open cost,
  // never a per-frame one. A file that exists but holds nothing is left out, so a dictionary
  // whose last note was deleted does not linger in the picker.
  static int listDictionaries(DictSummary* out, int cap);

  // --- Matching a note back onto a laid-out definition page ---------------------------------
  //
  // A note is matched as a SEQUENCE OF TOKENS, not as a byte run: the words a definition page
  // offers for selection are the ones DictionaryDefinitionActivity::extractWordsFromLayout
  // keeps, and it drops any token that cleans to nothing (a lone dash, a CJK full stop). So the
  // saved text can read "cat dog" where the page reads "cat - dog", and a byte search would
  // miss it. Matching token by token and skipping punctuation-only tokens in the page text
  // reproduces exactly what the user selected.
  //
  // Whitespace inside a segment and the break between segments are both treated as token
  // separators, so a note that wrapped across two lines still matches.

  // One piece of laid-out text: a page segment's string, in draw order.
  struct Segment {
    const char* text = nullptr;
    uint16_t len = 0;
  };

  // A run of one segment's bytes covered by a note. A note spanning a line break yields one
  // span per segment it touches.
  struct Span {
    uint16_t segIndex = 0;
    uint16_t byteStart = 0;
    uint16_t byteLen = 0;
  };

  // Locate every occurrence of `needle` across `segs` and write the covering spans to out[0..cap).
  // Returns how many spans were written (0 when the note is not on this page). Stops cleanly at
  // cap rather than dropping earlier spans, so a partial result is always a prefix.
  static int findSpans(const Segment* segs, int segCount, const char* needle, Span* out, int cap);
};
