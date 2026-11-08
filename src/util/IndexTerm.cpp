#include "IndexTerm.h"

#include <strings.h>

#include <algorithm>

#include <cstring>

namespace IndexTerm {
namespace {

bool isAsciiAlnum(const unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isDigits(const char* t, const size_t len) {
  if (len == 0 || len > 5) return false;
  for (size_t i = 0; i < len; i++) {
    if (t[i] < '0' || t[i] > '9') return false;
  }
  return true;
}

bool isRoman(const char* t, const size_t len) {
  if (len == 0 || len > 7) return false;
  for (size_t i = 0; i < len; i++) {
    if (!std::strchr("ivxlcdm", t[i])) return false;
  }
  return true;
}

// Byte length of a range dash at t[i] ('-', U+2013, U+2014), 0 when there is none.
size_t dashAt(const char* t, const size_t len, const size_t i) {
  if (t[i] == '-') return 1;
  if (i + 2 < len && static_cast<unsigned char>(t[i]) == 0xE2 && static_cast<unsigned char>(t[i + 1]) == 0x80 &&
      (static_cast<unsigned char>(t[i + 2]) == 0x93 || static_cast<unsigned char>(t[i + 2]) == 0x94)) {
    return 3;
  }
  return 0;
}

// "336-339" or "xi-xiv" as one token; `digitsOnly` rejects the roman form.
bool isRange(const char* t, const size_t len, const bool digitsOnly) {
  for (size_t i = 1; i < len; i++) {
    const size_t dash = dashAt(t, len, i);
    if (dash == 0) continue;
    const char* rhs = t + i + dash;
    const size_t rhsLen = len - i - dash;
    if (isDigits(t, i) && isDigits(rhs, rhsLen)) return true;
    return !digitsOnly && isRoman(t, i) && isRoman(rhs, rhsLen);
  }
  return false;
}

// One end of a range the layout split at its dash: "-47" (or "–47") after "146", or "146-" ending a
// line. A trailing dash takes digits only: "civil-" is a hyphenated word, not a roman numeral.
bool isRangeEdge(const char* t, const size_t len) {
  if (len < 2) return false;
  const size_t lead = dashAt(t, len, 0);
  // A word never starts with a dash, so a roman tail ("-xviii" of "xvii-xviii") is safe to read too.
  if (lead > 0) return isDigits(t + lead, len - lead) || isRoman(t + lead, len - lead);
  return t[len - 1] == '-' && isDigits(t, len - 1);
}

bool isDigitRef(const char* t, const size_t len) {
  return isDigits(t, len) || isRange(t, len, true) || isRangeEdge(t, len);
}

// Length of a word without the list punctuation a layout leaves on it ("Badawi," -> "Badawi").
bool isListPunct(const char c) { return c == ',' || c == ';' || c == ':' || c == '.'; }

// Bytes of a closing quote ending t[0, len) ('"', '\'', U+2019, U+201D), 0 when none.
size_t trailingQuoteLen(const char* t, const size_t len) {
  if (len >= 1 && (t[len - 1] == '"' || t[len - 1] == '\'')) return 1;
  if (len >= 3 && static_cast<unsigned char>(t[len - 3]) == 0xE2 && static_cast<unsigned char>(t[len - 2]) == 0x80 &&
      (static_cast<unsigned char>(t[len - 1]) == 0x99 || static_cast<unsigned char>(t[len - 1]) == 0x9D)) {
    return 3;
  }
  return 0;
}

// Length of a word without the list punctuation a layout leaves on it ("Badawi," -> "Badawi"). A
// quote closing over the comma goes with it: American style puts the comma inside ("\"Antennagate,\"").
size_t coreLen(const char* t, size_t len) {
  const size_t quote = trailingQuoteLen(t, len);
  if (quote > 0 && len > quote && isListPunct(t[len - quote - 1])) len -= quote;
  while (len > 0 && isListPunct(t[len - 1])) len--;
  return len;
}

bool endsWithListComma(const char* t, size_t len) {
  len -= trailingQuoteLen(t, len);
  for (size_t i = len; i > 0; i--) {
    const char c = t[i - 1];
    if (c == ',' || c == ';' || c == ':') return true;
    if (c != '.') return false;
  }
  return false;
}

bool isListComma(const Token& t) {
  return t.len == 1 && (t.text[0] == ',' || t.text[0] == ';' || t.text[0] == ':');
}

bool hasLetter(const char* t, const size_t len) {
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = static_cast<unsigned char>(t[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0) return true;
  }
  return false;
}

bool asciiEqualsIgnoreCase(const char* a, const size_t alen, const char* b) {
  if (std::strlen(b) != alen) return false;
  for (size_t i = 0; i < alen; i++) {
    char x = a[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (x != b[i]) return false;
  }
  return true;
}

// Joining words that open a term ("the Netherlands") or close a sub-entry ("media and").
bool isStopword(const char* t, const size_t len) {
  static constexpr const char* kStopwords[] = {"and",  "of",      "in",    "on",    "the",     "see",
                                               "for",  "with",    "as",    "to",    "by",      "at",
                                               "from", "about",   "vs",    "after", "against", "between",
                                               "into", "towards", "toward"};
  for (const char* s : kStopwords) {
    if (asciiEqualsIgnoreCase(t, len, s)) return true;
  }
  return false;
}

struct Word {
  const char* text;
  size_t len;
};

// A trailing word that says what kind of thing the term names ("chmod command", "AWK programming
// language", "PATH variable"): the text names the thing alone.
bool isCategoryWord(const char* t, const size_t len) {
  static constexpr const char* kCategories[] = {"command",  "commands", "program",  "programs",  "utility",
                                                "language", "variable", "variables", "function", "functions",
                                                "statement", "option",  "options",  "operator", "file",
                                                "package",  "tool",     "builtin",  "keyword"};
  for (const char* c : kCategories) {
    if (asciiEqualsIgnoreCase(t, len, c)) return true;
  }
  return false;
}

// Byte length of an opening quote at the start of w ('"', '\'', U+2018, U+201C), 0 when none.
size_t openQuoteLen(const char* t, const size_t len) {
  if (len >= 1 && (t[0] == '"' || t[0] == '\'')) return 1;
  if (len >= 3 && static_cast<unsigned char>(t[0]) == 0xE2 && static_cast<unsigned char>(t[1]) == 0x80 &&
      (static_cast<unsigned char>(t[2]) == 0x98 || static_cast<unsigned char>(t[2]) == 0x9C)) {
    return 3;
  }
  return 0;
}

// Byte length of a closing quote at the end of w ('"', '\'', U+2019, U+201D), 0 when none.
size_t closeQuoteLen(const char* t, const size_t len) {
  if (len >= 1 && (t[len - 1] == '"' || t[len - 1] == '\'')) return 1;
  if (len >= 3 && static_cast<unsigned char>(t[len - 3]) == 0xE2 && static_cast<unsigned char>(t[len - 2]) == 0x80 &&
      (static_cast<unsigned char>(t[len - 1]) == 0x99 || static_cast<unsigned char>(t[len - 1]) == 0x9D)) {
    return 3;
  }
  return 0;
}

// Appends `w` to out (space-separated), keeping the result NUL-terminated within `cap`.
bool appendWord(char* out, const size_t cap, size_t& used, const char* w, const size_t len, const bool space) {
  const size_t need = len + (space ? 1 : 0);
  if (used + need + 1 > cap) return false;
  if (space) out[used++] = ' ';
  std::memcpy(out + used, w, len);
  used += len;
  out[used] = '\0';
  return true;
}

// Appends words[from, to) to out, dropping separators.
void appendWords(const Word* words, const size_t from, const size_t to, char* out, const size_t cap, size_t& used) {
  for (size_t i = from; i < to; i++) {
    if (isSeparator(words[i].text, words[i].len)) continue;
    if (!appendWord(out, cap, used, words[i].text, words[i].len, used > 0)) return;
  }
}

// Joins words[from, to) without separators; true when the result has at least one letter.
bool joinWords(const Word* words, const size_t from, const size_t to, char* out, const size_t cap) {
  size_t used = 0;
  out[0] = '\0';
  appendWords(words, from, to, out, cap, used);
  return used > 0 && hasLetter(out, used);
}

// An abbreviation in capitals ("ANC", "WP"): short, but chosen to name the entry, and matched as a
// whole word, so it cannot hit inside a longer one.
bool isAbbreviation(const char* text) {
  size_t len = 0;
  bool letter = false;
  for (; text[len]; len++) {
    const char c = text[len];
    if (c >= 'A' && c <= 'Z') {
      letter = true;
    } else if (!(c >= '0' && c <= '9')) {
      return false;
    }
  }
  return letter && len >= 2;
}

// A capitalised word of three letters is still a name to search for ("Ali" of "Ali, Muhammad").
bool isShortName(const char* text) {
  return std::strlen(text) == 3 && text[0] >= 'A' && text[0] <= 'Z' && hasLetter(text + 1, 2);
}

// A name rather than a common word: a capital (or a non-ASCII letter, whose case is not checked).
bool isCapitalised(const Word& w) {
  const unsigned char c = static_cast<unsigned char>(w.text[0]);
  return w.len > 0 && ((c >= 'A' && c <= 'Z') || c >= 0xC0);
}

// `minLen` lowers the length floor for a candidate known to be a name in its own right ("cat" of
// "cat command").
bool addCandidate(const char* text, char (*out)[kMaxTerm], size_t& count, const size_t maxOut,
                  const bool allowShortName = false, const size_t minLen = kMinCandidateLen) {
  if (count >= maxOut) return false;
  if (std::strlen(text) < minLen && !isAbbreviation(text) && !(allowShortName && isShortName(text))) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    if (strcasecmp(out[i], text) == 0) return false;
  }
  std::strncpy(out[count], text, kMaxTerm - 1);
  out[count][kMaxTerm - 1] = '\0';
  count++;
  return true;
}

// The line holding tokens[at] is a cross-reference entry ("Apple products, see individual product
// names"): it has no page reference to end it, so it would otherwise run into the next entry's term.
bool isCrossReferenceLine(const Token* tokens, const size_t at) {
  const uint16_t line = tokens[at].line;
  size_t k = at;
  while (k > 0 && tokens[k - 1].line == line) k--;
  for (; k <= at; k++) {
    const Token& t = tokens[k];
    const size_t core = coreLen(t.text, t.len);
    const char* text = t.text;
    size_t len = core;
    if (len > 0 && text[0] == '(') {
      text++;
      len--;
    }
    if (!asciiEqualsIgnoreCase(text, len, "see")) continue;
    // "Term, see ..." or "Term (see ...)": a "see" inside a title has neither before it.
    if (text != t.text) return true;
    if (k > 0 && tokens[k - 1].line == line &&
        (isListComma(tokens[k - 1]) || endsWithListComma(tokens[k - 1].text, tokens[k - 1].len))) {
      return true;
    }
  }
  return false;
}

// isCrossReferenceLine for the line of tokens[at] or a line it wraps on from: a cross-reference can
// span lines ("Apple products, see individual | product names"), and none of them ends in a reference.
bool isCrossReferenceChain(const Token* tokens, size_t at) {
  for (int lines = 0; lines < 3; lines++) {
    if (isCrossReferenceLine(tokens, at)) return true;
    size_t s = at;
    while (s > 0 && tokens[s - 1].line == tokens[at].line) s--;
    if (s == 0) return false;
    const Token& end = tokens[s - 1];
    if (isPageRef(end.text, coreLen(end.text, end.len)) || isSeparator(end.text, end.len)) return false;
    at = s - 1;
  }
  return false;
}

size_t lineStart(const Token* tokens, size_t at) {
  while (at > 0 && tokens[at - 1].line == tokens[at].line) at--;
  return at;
}

bool startsCapitalised(const Token& t) {
  const unsigned char c = static_cast<unsigned char>(t.text[0]);
  return t.len > 0 && ((c >= 'A' && c <= 'Z') || c >= 0xC0);
}

}  // namespace

bool isPageRef(const char* text, const size_t len) {
  return isDigits(text, len) || isRoman(text, len) || isRange(text, len, false) || isRangeEdge(text, len);
}

bool isPageRefLabel(const char* label) {
  bool ref = false;
  for (const char* p = label; *p;) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ') p++;
    const size_t len = static_cast<size_t>(p - start);
    if (isSeparator(start, len)) continue;
    if (!isPageRef(start, coreLen(start, len))) return false;
    ref = true;
  }
  return ref;
}

bool isSeparator(const char* text, const size_t len) {
  if (len == 0) return true;
  for (size_t i = 0; i < len;) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (isAsciiAlnum(c)) return false;
      i++;
    } else if (c == 0xE2 && i + 2 < len && static_cast<unsigned char>(text[i + 1]) == 0x80) {
      i += 3;  // General Punctuation: dashes, quotes, ellipsis
    } else {
      return false;
    }
  }
  return true;
}

bool looksLikeIndex(const size_t linkCount, const size_t pageRefLinks) {
  return linkCount >= 6 && pageRefLinks * 10 >= linkCount * 6;
}

bool extractTerm(const Token* tokens, const size_t count, const size_t linkToken, char* out, const size_t cap,
                 size_t* firstOut) {
  if (cap == 0) return false;
  out[0] = '\0';
  if (linkToken >= count) return false;
  const Token& link = tokens[linkToken];
  if (!isPageRef(link.text, coreLen(link.text, link.len))) return false;

  // Back over the entry's earlier references to the term; a list comma must come between. Without
  // focus reading the layout keeps punctuation on its word ("Badawi,", "207,"), with it the comma
  // is a token of its own: both forms count.
  size_t j = linkToken;
  bool sawComma = false;
  size_t lastLen = 0;  // the term's last word without its comma
  while (j > 0) {
    const Token& t = tokens[j - 1];
    const size_t core = coreLen(t.text, t.len);
    const bool comma = endsWithListComma(t.text, t.len);
    if (isSeparator(t.text, t.len) || isPageRef(t.text, core)) {
      sawComma |= comma;
      j--;
      continue;
    }
    sawComma |= comma;
    lastLen = comma ? core : t.len;
    break;
  }
  if (j == 0 || !sawComma) return false;
  const size_t last = j - 1;

  // Back over the term to the entry's start: the previous entry ends in a page reference, a
  // letter heading ("D") is a line of its own, and a term only wraps onto a line that does not
  // end in a reference.
  constexpr size_t kMaxTermWords = 10;
  size_t first = last;
  size_t words = 1;
  while (first > 0 && words < kMaxTermWords) {
    const Token& prev = tokens[first - 1];
    const size_t prevCore = coreLen(prev.text, prev.len);
    if (prev.line != tokens[first].line) {
      size_t onLine = 0;
      for (size_t k = first; k > 0 && tokens[k - 1].line == prev.line; k--) onLine++;
      if (onLine < 2 || isPageRef(prev.text, prevCore) || isSeparator(prev.text, prev.len)) break;
      if (isCrossReferenceChain(tokens, first - 1)) break;
      // "Jobs, Steve:" heading its sub-entries: a term wraps mid-phrase, never after its own comma.
      if (endsWithListComma(prev.text, prev.len)) break;
    }
    if (isDigitRef(prev.text, prevCore)) break;
    // "..., xix" closing the previous entry; a roman-letter word inside a term has no comma before it.
    if (isRoman(prev.text, prevCore) && first >= 2 &&
        (isListComma(tokens[first - 2]) || endsWithListComma(tokens[first - 2].text, tokens[first - 2].len))) {
      break;
    }
    first--;
    words++;
  }
  while (first < last && isSeparator(tokens[first].text, tokens[first].len)) first++;
  if (firstOut) *firstOut = first;

  size_t used = 0;
  bool joinNext = false;  // the previous token was a line-end hyphenation prefix
  for (size_t k = first; k <= last; k++) {
    const Token& t = tokens[k];
    size_t len = k == last ? lastLen : t.len;
    const bool brokenWord = k < last && len > 1 && t.text[len - 1] == '-' && tokens[k + 1].line != t.line;
    if (brokenWord) len--;
    if (len > 0 && !appendWord(out, cap, used, t.text, len, used > 0 && !joinNext)) break;
    joinNext = brokenWord;
  }
  return used > 0 && hasLetter(out, used);
}

namespace {
size_t buildTermCandidates(const char* term, char (*out)[kMaxTerm], size_t maxOut, bool phraseOnly = false);
}  // namespace

bool extractParent(const Token* tokens, const size_t count, const size_t first, char* out, const size_t cap) {
  if (cap == 0) return false;
  out[0] = '\0';
  if (first >= count) return false;
  // The heading sits out to the left of its sub-entries (or, run in, on the line its sub-entries
  // continue from): the nearest line at each smaller indent, walking up, ends on it.
  constexpr int kIndentSlack = 2;
  const size_t own = lineStart(tokens, first);
  int indent = tokens[own].x;
  size_t parent = first != own ? own : SIZE_MAX;
  for (size_t s = own; s > 0;) {
    s = lineStart(tokens, s - 1);
    if (tokens[s].x + kIndentSlack < indent) {
      parent = s;
      indent = tokens[s].x;
    }
  }
  if (parent == SIZE_MAX || parent == first || !startsCapitalised(tokens[parent])) return false;

  // Its term: the words up to its first list comma or reference, on its own line.
  constexpr size_t kMaxParentWords = 6;
  size_t used = 0;
  size_t words = 0;
  for (size_t k = parent; k < count && words < kMaxParentWords; k++, words++) {
    const Token& t = tokens[k];
    if (t.line != tokens[parent].line || k == first) break;
    const size_t core = coreLen(t.text, t.len);
    if (isSeparator(t.text, t.len) || isPageRef(t.text, core)) break;
    if (!appendWord(out, cap, used, t.text, core, used > 0)) break;
    if (endsWithListComma(t.text, t.len) || (k + 1 < count && isListComma(tokens[k + 1]))) {
      words++;
      break;
    }
  }
  // One word is the book's subject as often as not ("Jobs" of "Jobs, Steve:"), on every page near
  // any anchor: searching it would box an unrelated mention.
  if (words < 2) {
    out[0] = '\0';
    return false;
  }
  return used > 0 && hasLetter(out, used);
}

size_t buildCandidates(const char* entry, char (*out)[kMaxTerm], const size_t maxOut) {
  char term[kMaxTerm];
  const char* sep = std::strchr(entry, kParentSeparator);
  const size_t termLen = std::min(sep ? static_cast<size_t>(sep - entry) : std::strlen(entry), kMaxTerm - 1);
  std::memcpy(term, entry, termLen);
  term[termLen] = '\0';
  size_t count = buildTermCandidates(term, out, maxOut);
  if (!sep || count >= maxOut) return count;
  // A sub-entry is the indexer's summary ("checkout design of"): the text may carry only its key
  // word ("checkout"). Its longest word then, even a common one -- the search stays within about a
  // printed page of the cited one, where the sub-entry says it is discussed.
  constexpr size_t kMinKeyWord = 5;
  const char* key = nullptr;
  size_t keyLen = 0;
  for (const char* p = term; *p;) {
    while (*p == ' ') p++;
    const char* start = p;
    while (*p && *p != ' ') p++;
    size_t len = coreLen(start, static_cast<size_t>(p - start));
    const char* w = start;
    const size_t oq = openQuoteLen(w, len);
    w += oq;
    len -= oq;
    len -= trailingQuoteLen(w, len);
    if (len > keyLen && len >= kMinKeyWord && len < kMaxTerm && !isStopword(w, len) && hasLetter(w, len)) {
      key = w;
      keyLen = len;
    }
  }
  if (key) {
    char buf[kMaxTerm];
    std::memcpy(buf, key, keyLen);
    buf[keyLen] = '\0';
    addCandidate(buf, out, count, maxOut);
  }
  if (count >= maxOut) return count;
  // Failing that, its heading ("Apple III computer") at least finds the passage: its phrase and
  // opening words, after the sub-entry's own.
  char parentCands[2][kMaxTerm];
  const size_t parentCount = buildTermCandidates(sep + 1, parentCands, 2, /*phraseOnly=*/true);
  for (size_t i = 0; i < parentCount && count < maxOut; i++) addCandidate(parentCands[i], out, count, maxOut);
  return count;
}

namespace {
// The candidates of one term; `phraseOnly` keeps the phrase and its opening words, for a heading.
size_t buildTermCandidates(const char* term, char (*out)[kMaxTerm], const size_t maxOut, const bool phraseOnly) {
  constexpr size_t kMaxWords = 10;  // two word tables + one term buffer stay under the stack budget
  Word mainWords[kMaxWords];
  Word parenWords[kMaxWords];
  size_t mainCount = 0;
  size_t parenCount = 0;
  size_t commaAfter = SIZE_MAX;  // main words before the first comma ("Hawke," / "Hawke ,")
  int parenDepth = 0;
  for (const char* p = term; *p;) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ') p++;
    // Peel the punctuation a word may carry: "(ANC),", "(Chinese", "Party)", "Hawke,".
    Word w{start, static_cast<size_t>(p - start)};
    const bool comma = endsWithListComma(w.text, w.len);
    w.len = coreLen(w.text, w.len);
    // A quoted title ("\"All Watched Over ... Grace\""): the page's words carry the quotes too, but
    // the search drops a page word's punctuation, so the term's must go as well.
    const size_t oq = openQuoteLen(w.text, w.len);
    w.text += oq;
    w.len -= oq;
    w.len -= closeQuoteLen(w.text, w.len);
    w.len = coreLen(w.text, w.len);
    const bool opens = w.len > 0 && w.text[0] == '(';
    if (opens) {
      w.text++;
      w.len--;
    }
    const bool closes = w.len > 0 && w.text[w.len - 1] == ')';
    if (closes) w.len--;

    if (opens) parenDepth++;
    if (w.len > 0) {
      if (parenDepth > 0) {
        if (parenCount < kMaxWords) parenWords[parenCount++] = w;
      } else if (mainCount < kMaxWords && !isSeparator(w.text, w.len)) {
        mainWords[mainCount++] = w;
      }
    }
    if (closes && parenDepth > 0) parenDepth--;
    if (comma && parenDepth == 0 && commaAfter == SIZE_MAX) commaAfter = mainCount;
  }
  size_t lead = 0;
  while (lead < mainCount && isStopword(mainWords[lead].text, mainWords[lead].len)) lead++;
  // A sub-entry ("media and", "Macworld gaffe of") reads on from its heading, never as printed.
  size_t tail = mainCount;
  while (tail > lead && isStopword(mainWords[tail - 1].text, mainWords[tail - 1].len)) tail--;
  const bool subEntry = tail < mainCount;
  const bool inverted = commaAfter != SIZE_MAX && commaAfter > lead && commaAfter < tail;

  size_t count = 0;
  char buf[kMaxTerm];
  if (inverted) {
    // "Hawke, Bob": the text says "Bob Hawke".
    size_t used = 0;
    buf[0] = '\0';
    appendWords(mainWords, commaAfter, tail, buf, sizeof(buf), used);
    appendWords(mainWords, lead, commaAfter, buf, sizeof(buf), used);
    if (used > 0 && hasLetter(buf, used)) addCandidate(buf, out, count, maxOut);
  } else if (joinWords(mainWords, lead, tail, buf, sizeof(buf))) {
    addCandidate(buf, out, count, maxOut);
    // An index names things in the plural ("Academy Awards", "prototypes") that the text mentions
    // one at a time. Not "-ss" or "-us" ("glass", "status"), which are not plurals.
    const size_t len = std::strlen(buf);
    if (!phraseOnly && len > 4 && buf[len - 1] == 's' && buf[len - 2] != 's' && buf[len - 2] != 'u' &&
        buf[len - 2] != '\'') {
      buf[len - 1] = '\0';
      addCandidate(buf, out, count, maxOut);
    }
  }
  if (!phraseOnly && joinWords(parenWords, 0, parenCount, buf, sizeof(buf))) addCandidate(buf, out, count, maxOut);

  // The thing without its category ("chmod" of "chmod command", "AWK" of "AWK programming language").
  if (!inverted && !phraseOnly && tail - lead >= 2 && isCategoryWord(mainWords[tail - 1].text, mainWords[tail - 1].len)) {
    size_t end = tail - 1;
    // "programming language": the qualifier goes with the category.
    if (end - lead >= 2 && asciiEqualsIgnoreCase(mainWords[end - 1].text, mainWords[end - 1].len, "programming")) end--;
    if (end > lead && joinWords(mainWords, lead, end, buf, sizeof(buf))) {
      addCandidate(buf, out, count, maxOut, false, /*minLen=*/2);
    }
  }

  // The opening two words of a longer phrase ("Apple I" of "Apple I computer", "stock options" of
  // "stock options controversy"): an entry often adds a word the text leaves out.
  if (!inverted && tail - lead >= 3) {
    size_t end = lead + 2;
    while (end > lead && isStopword(mainWords[end - 1].text, mainWords[end - 1].len)) end--;
    if (end - lead >= 2 && joinWords(mainWords, lead, end, buf, sizeof(buf))) addCandidate(buf, out, count, maxOut);
  }

  // One word alone, only when it is a name: the last word before an inversion comma, else the
  // term's last word ("Hawke", "Bureau"); a sub-entry's last word is as often a filler ("deal"),
  // so it takes its longest name. Not an earlier word -- "Kuan" of "Lee Kuan Yew" names someone
  // else as often as not -- and never a common word ("computer"), which any page near the anchor
  // holds and would box in the wrong place.
  const size_t surnameEnd = inverted ? commaAfter : tail;
  if (!phraseOnly && surnameEnd > lead) {
    size_t pick = surnameEnd - 1;
    if (subEntry) {
      for (size_t i = lead; i < surnameEnd; i++) {
        if (isCapitalised(mainWords[i]) && (!isCapitalised(mainWords[pick]) || mainWords[i].len > mainWords[pick].len)) {
          pick = i;
        }
      }
    }
    const Word& w = mainWords[pick];
    if (isCapitalised(w) && w.len >= 3 && w.len < kMaxTerm && hasLetter(w.text, w.len)) {
      std::memcpy(buf, w.text, w.len);
      buf[w.len] = '\0';
      addCandidate(buf, out, count, maxOut, /*allowShortName=*/inverted);
    }
  }
  return count;
}
}  // namespace

}  // namespace IndexTerm
