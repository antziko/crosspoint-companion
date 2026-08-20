#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string_view>

// Curated English closed-class stopword list for the automatic-capture filter.
// These are the function words (articles, prepositions, pronouns, conjunctions,
// core auxiliaries/modals) that clutter the per-book lookup history and
// flashcard deck with no value as study material. The dictionary still LOOKS UP
// and DISPLAYS these words when tapped; they are only excluded from being
// captured. Two choke points, which must stay in agreement — a word kept out of
// history but enrolled as a card is the two lists disagreeing about one lookup:
//   * LookupHistory::addWordIf  (per-book lookup log)
//   * FlashcardDeck::enroll     (per-book deck)
//
// Design constraints (CLAUDE.md):
//   * Flash-resident, zero DRAM: an array of `const char*` literals marked
//     `constexpr` lives entirely in flash. No heap, no SD I/O at lookup time.
//   * No POS tagger exists on-device, so this is a hand-curated CLOSED-CLASS
//     list, not part-of-speech detection. It is English-only.
//   * Conservative on purpose: surface-ambiguous words with a common content
//     sense are deliberately OMITTED so a real lookup is never silently dropped
//     (e.g. will/can/may/might/must, well, like, back, right, since, down, up,
//     out, off, over, near, past, mine). Add such words only if their function
//     reading clearly dominates.
//
// Filtering happens at record time only; the cross-device sync merge path
// (LookupHistory::mergeBlob) is intentionally NOT filtered, so a peer on older
// firmware can still reintroduce a stopword without breaking Lamport
// convergence. That is an accepted trade-off.
//
// The array MUST stay sorted ascending (byte/strcmp order, all lowercase) for
// the binary search below. test/dict-stopwords asserts this.
namespace DictStopwords {

inline constexpr const char* const STOPWORDS[] = {
    "a",          "about",      "above",     "across",     "after",      "against",  "all",        "alongside",
    "although",   "am",         "amid",      "among",      "amongst",    "an",       "and",        "another",
    "any",        "anybody",    "anyone",    "anything",   "anywhere",   "are",      "around",     "as",
    "at",         "be",         "because",   "been",       "before",     "behind",   "being",      "below",
    "beneath",    "beside",     "between",   "beyond",     "both",       "but",      "by",         "could",
    "despite",    "did",        "do",        "does",       "during",     "each",     "either",     "enough",
    "every",      "everybody",  "everyone",  "everything", "everywhere", "except",   "few",        "fewer",
    "for",        "from",       "had",       "has",        "have",       "he",       "her",        "hers",
    "herself",    "him",        "himself",   "his",        "how",        "i",        "if",         "in",
    "inside",     "into",       "is",        "it",         "its",        "itself",   "many",       "me",
    "more",       "most",       "much",      "my",         "myself",     "neither",  "nobody",     "none",
    "nor",        "not",        "nothing",   "nowhere",    "of",         "on",       "onto",       "or",
    "our",        "ours",       "ourselves", "outside",    "per",        "plus",     "several",    "shall",
    "she",        "should",     "so",        "some",       "somebody",   "someone",  "something",  "somewhere",
    "such",       "than",       "that",      "the",        "their",      "theirs",   "them",       "themselves",
    "these",      "they",       "this",      "those",      "though",     "through",  "throughout", "to",
    "toward",     "towards",    "under",     "underneath", "unless",     "unlike",   "until",      "upon",
    "us",         "various",    "via",       "was",        "we",         "were",     "what",       "whatever",
    "whatsoever", "when",       "whenever",  "where",      "whereas",    "wherever", "whether",    "which",
    "whichever",  "while",      "who",       "whoever",    "whom",       "whomever", "whose",      "why",
    "with",       "within",     "without",   "would",      "yet",        "you",      "your",       "yours",
    "yourself",   "yourselves",
};

inline constexpr size_t STOPWORD_COUNT = sizeof(STOPWORDS) / sizeof(STOPWORDS[0]);

// Longest entry is 10 chars; a 16-byte stack buffer bounds the lowercase copy
// and lets us early-out on any token that cannot possibly be a stopword.
inline constexpr size_t MAX_STOPWORD_LEN = 15;

// True iff `word[0..len)`, lowercased, is in STOPWORDS. No heap; `word` need not
// be null-terminated (only the first `len` bytes are read), so this is safe on a
// std::string_view substring.
inline bool isStopword(const char* word, size_t len) {
  if (len == 0 || len > MAX_STOPWORD_LEN) return false;
  char buf[MAX_STOPWORD_LEN + 1];
  for (size_t i = 0; i < len; ++i) {
    buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(word[i])));
  }
  buf[len] = '\0';
  const char* const* end = STOPWORDS + STOPWORD_COUNT;
  const char* const* it =
      std::lower_bound(STOPWORDS, end, buf, [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
  return it != end && std::strcmp(*it, buf) == 0;
}

inline bool isStopword(std::string_view word) { return isStopword(word.data(), word.size()); }

}  // namespace DictStopwords
