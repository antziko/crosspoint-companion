// Host-side litmus for PageTokenScan — the page word-index space.
//
// A quote stores its selection as page-local word indices, so the numbering
// DictionaryWordSelectActivity::extractWords produces and the numbering the reader's quote
// underline walks must agree token for token. The rule they share is collectParts(), and this
// suite pins it against an oracle that is the original inline split loop, verbatim: if the two
// ever disagree, every stored highlight silently shifts onto the wrong words.
//
// Build/run: test/page-token-scan/run.sh

#include <GfxRenderer.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "util/PageTokenScan.h"
#include "util/QuoteSpan.h"
#include "util/SnippetMatch.h"

// --------------------------------------------------------------------------
// Tiny test harness
// --------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) {                                                    \
      ++g_failures;                                                   \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    }                                                                 \
  } while (0)

// --------------------------------------------------------------------------
// Oracle: the dash-split loop exactly as extractWords carried it inline, including its
// "single start at offset 0 means the token was never split" branch.
// --------------------------------------------------------------------------
static std::vector<std::pair<size_t, size_t>> oracleParts(const std::string& t) {
  const auto isDash = [&](size_t i) {
    return i + 2 < t.size() && static_cast<uint8_t>(t[i]) == 0xE2 && static_cast<uint8_t>(t[i + 1]) == 0x80 &&
           (static_cast<uint8_t>(t[i + 2]) == 0x93 || static_cast<uint8_t>(t[i + 2]) == 0x94);
  };

  std::vector<size_t> splitStarts;
  size_t partStart = 0;
  for (size_t i = 0; i < t.size();) {
    if (isDash(i)) {
      if (i > partStart) splitStarts.push_back(partStart);
      i += 3;
      partStart = i;
    } else {
      i++;
    }
  }
  if (partStart < t.size()) splitStarts.push_back(partStart);

  std::vector<std::pair<size_t, size_t>> out;
  if (splitStarts.size() <= 1 && partStart == 0) {
    out.emplace_back(0, t.size());
    return out;
  }
  for (size_t si = 0; si < splitStarts.size(); si++) {
    const size_t start = splitStarts[si];
    size_t textEnd = (si + 1 < splitStarts.size()) ? splitStarts[si + 1] : t.size();
    while (textEnd > start) {
      if (textEnd >= 3 && static_cast<uint8_t>(t[textEnd - 3]) == 0xE2 &&
          static_cast<uint8_t>(t[textEnd - 2]) == 0x80 &&
          (static_cast<uint8_t>(t[textEnd - 1]) == 0x93 || static_cast<uint8_t>(t[textEnd - 1]) == 0x94)) {
        textEnd -= 3;
      } else {
        break;
      }
    }
    if (textEnd == start) continue;  // a part that was nothing but dashes
    out.emplace_back(start, textEnd);
  }
  return out;
}

static const char* EN = "\xE2\x80\x93";  // U+2013
static const char* EM = "\xE2\x80\x94";  // U+2014

// --------------------------------------------------------------------------
// collectParts agrees with the loop it replaced
// --------------------------------------------------------------------------
static void testPartsMatchOriginalSplit() {
  std::printf("collectParts matches the original inline split\n");

  const std::vector<std::string> corpus = {
      "hello",                                  // no dash: one part, whole token
      std::string("east") + EN + "west",        // the ordinary case
      std::string("a") + EM + "b" + EN + "c",   // several, mixed kinds
      std::string(EN) + "leading",              // dash first
      std::string("trailing") + EM,             // dash last
      std::string("double") + EN + EN + "gap",  // adjacent dashes leave an empty part
      std::string(EN),                          // nothing but a dash
      std::string(EN) + EM,                     // nothing but dashes
      std::string("中") + EN + "文",            // multi-byte either side
      "co-operate",                             // ASCII hyphen is not a dash
      "\xC2\xAD"
      "soft",  // soft hyphen is not a dash
  };

  // An empty token is deliberately absent: isSelectable rejects it, so it never reaches a split.
  bool cjk = false;
  CHECK(!PageTokens::isSelectable("", 0, cjk), "an empty token is never selectable");

  for (const auto& t : corpus) {
    const auto expected = oracleParts(t);
    PageTokens::Part got[PageTokens::kMaxTokenParts];
    const size_t n = PageTokens::collectParts(t.data(), t.size(), got, PageTokens::kMaxTokenParts);

    char msg[160];
    std::snprintf(msg, sizeof(msg), "part count for \"%s\": got %u, expected %u", t.c_str(), (unsigned)n,
                  (unsigned)expected.size());
    CHECK(n == expected.size(), msg);
    if (n != expected.size()) continue;

    for (size_t i = 0; i < n; i++) {
      std::snprintf(msg, sizeof(msg), "part %u of \"%s\": got [%u,%u), expected [%u,%u)", (unsigned)i, t.c_str(),
                    (unsigned)got[i].start, (unsigned)got[i].end, (unsigned)expected[i].first,
                    (unsigned)expected[i].second);
      CHECK(got[i].start == expected[i].first && got[i].end == expected[i].second, msg);
    }
  }
}

// An unsplit token has to be recognisable as such, because that is the branch extractWords
// derives its width from the layout's xpos diff on rather than measuring.
static void testUnsplitTokenIsWholeToken() {
  std::printf("an undashed token yields exactly one whole-token part\n");

  const std::string t = "ordinary";
  PageTokens::Part parts[PageTokens::kMaxTokenParts];
  const size_t n = PageTokens::collectParts(t.data(), t.size(), parts, PageTokens::kMaxTokenParts);
  CHECK(n == 1, "one part");
  CHECK(n == 1 && parts[0].start == 0 && parts[0].end == t.size(), "spanning the whole token");

  // A dashed token must NOT look unsplit, or the caller measures the wrong span.
  const std::string dashed = std::string("east") + EN + "west";
  const size_t dn = PageTokens::collectParts(dashed.data(), dashed.size(), parts, PageTokens::kMaxTokenParts);
  CHECK(dn == 2, "dashed token splits in two");
  CHECK(dn == 2 && !(parts[0].start == 0 && parts[0].end == dashed.size()), "first part is not the whole token");
}

// --------------------------------------------------------------------------
// ASCII double hyphen
//
// A run of two or more '-' is the typewriter em-dash, and splits like one. The oracle above
// is the ORIGINAL loop and deliberately does not know this rule, so these expectations are
// written out by hand: they are the one place the page index space was moved on purpose.
// A single '-' must keep behaving as a compound-word hyphen.
// --------------------------------------------------------------------------
static void testAsciiDoubleHyphenSplits() {
  std::printf("a run of ASCII hyphens splits like an em-dash\n");

  struct Case {
    std::string token;
    std::vector<std::pair<size_t, size_t>> expected;
    const char* what;
  };
  const std::vector<Case> cases = {
      {"east--west", {{0, 4}, {6, 10}}, "the reported case: two words, dashes owned by neither"},
      {"east---west", {{0, 4}, {7, 11}}, "three hyphens are still one separator"},
      {"word--", {{0, 4}}, "a trailing run yields the word alone, so it is not a line-break hyphen"},
      {"--word", {{2, 6}}, "a leading run is skipped"},
      {"a--b--c", {{0, 1}, {3, 4}, {6, 7}}, "several runs in one token"},
      {"co-operate", {{0, 10}}, "a single hyphen is a compound word, not a separator"},
      {"well-known--thing", {{0, 10}, {12, 17}}, "single and double hyphens in one token"},
      {"--", {}, "nothing but separators yields no index"},
  };

  for (const auto& c : cases) {
    PageTokens::Part got[PageTokens::kMaxTokenParts];
    const size_t n = PageTokens::collectParts(c.token.data(), c.token.size(), got, PageTokens::kMaxTokenParts);

    char msg[200];
    std::snprintf(msg, sizeof(msg), "%s -- \"%s\": got %u parts, expected %u", c.what, c.token.c_str(), (unsigned)n,
                  (unsigned)c.expected.size());
    CHECK(n == c.expected.size(), msg);
    if (n != c.expected.size()) continue;

    for (size_t i = 0; i < n; i++) {
      std::snprintf(msg, sizeof(msg), "part %u of \"%s\": got [%u,%u), expected [%u,%u)", (unsigned)i, c.token.c_str(),
                    (unsigned)got[i].start, (unsigned)got[i].end, (unsigned)c.expected[i].first,
                    (unsigned)c.expected[i].second);
      CHECK(got[i].start == c.expected[i].first && got[i].end == c.expected[i].second, msg);
    }
  }

  // The split token must not look unsplit, or extractWords takes the whole-token width.
  const std::string t = "east--west";
  PageTokens::Part parts[PageTokens::kMaxTokenParts];
  const size_t n = PageTokens::collectParts(t.data(), t.size(), parts, PageTokens::kMaxTokenParts);
  CHECK(n == 2 && !(parts[0].start == 0 && parts[0].end == t.size()), "a dashed token never looks unsplit");

  // countDashes bounds the reservation: exact when the token has content on both sides of
  // every separator, one high when a run trails, never low.
  CHECK(PageTokens::countDashes("east--west", 10) == 1, "one separator counted for a two-hyphen run");
  CHECK(PageTokens::countDashes("east---west", 11) == 1, "a three-hyphen run is still one separator");
  CHECK(PageTokens::countDashes("a--b--c", 7) == 2, "two separators counted");
  CHECK(PageTokens::countDashes("co-operate", 10) == 0, "a single hyphen is not counted");
  CHECK(PageTokens::countDashes("word--", 6) >= 1, "a trailing run is counted, erring high is fine");

  // The whole point of the trailing-run case: the part carries no hyphen, so the
  // hyphenated-pair merge cannot mistake "word--" at a line end for a line-break hyphen.
  const std::string trailing = "word--";
  const size_t tn = PageTokens::collectParts(trailing.data(), trailing.size(), parts, PageTokens::kMaxTokenParts);
  CHECK(tn == 1 && trailing.substr(parts[0].start, parts[0].end - parts[0].start) == "word",
        "the emitted part is hyphen-free, so no bogus continuation merge");
}

// --------------------------------------------------------------------------
// Selection predicate
// --------------------------------------------------------------------------
static void testSelectablePredicate() {
  std::printf("isSelectable accepts content, rejects punctuation\n");

  bool cjk = false;
  CHECK(PageTokens::isSelectable("hello", 5, cjk), "latin word is selectable");
  CHECK(!cjk, "latin word is not CJK");

  CHECK(PageTokens::isSelectable("42", 2, cjk), "digits are selectable");

  CHECK(!PageTokens::isSelectable("...", 3, cjk), "ascii punctuation is not selectable");
  CHECK(!PageTokens::isSelectable("\xE2\x80\x9C", 3, cjk), "a curly quote is not selectable");

  CHECK(PageTokens::isSelectable("中", 3, cjk), "han character is selectable");
  CHECK(cjk, "han character reports CJK");

  CHECK(!PageTokens::isSelectable("。", 3, cjk), "CJK punctuation is not selectable");
  CHECK(!cjk, "CJK punctuation does not report CJK");
}

// --------------------------------------------------------------------------
// Measurement
// --------------------------------------------------------------------------
static void testMeasureStripsSoftHyphen() {
  std::printf("measureAdvance measures what layout drew\n");

  GfxRenderer renderer;
  // Layout strips soft hyphens before measuring, so a width that kept them would overrun the
  // word — and the underline with it.
  const int16_t w = PageTokens::measureAdvance(renderer, 0,
                                               "ab\xC2\xAD"
                                               "cd",
                                               6, EpdFontFamily::REGULAR);
  CHECK(std::strcmp(renderer.lastMeasured, "abcd") == 0, "soft hyphen removed before measuring");
  CHECK(w == 4, "width is the sanitized length");

  // The pointer+length form must measure only the range it was given, not the rest of the
  // token: dash-split parts and prefix offsets both depend on it.
  const int16_t part = PageTokens::measureAdvance(renderer, 0, "abcdef", 3, EpdFontFamily::REGULAR);
  CHECK(std::strcmp(renderer.lastMeasured, "abc") == 0, "only the given range is measured");
  CHECK(part == 3, "width of the given range");
}

// --------------------------------------------------------------------------
// nextTextPart: the same two rules applied to a plain string, so a stored snippet can be
// matched against a page token by token. A disagreement here means a highlight made on a
// peer never draws -- the page hands over tokens the snippet walk is not expecting.
// --------------------------------------------------------------------------
static std::vector<std::string> textParts(const char* text) {
  std::vector<std::string> out;
  size_t from = 0, start = 0, len = 0, next = 0;
  while (PageTokens::nextTextPart(text, from, start, len, next)) {
    out.emplace_back(text + start, len);
    from = next;
  }
  return out;
}

// The page walk's own sequence for the same text, one whitespace-delimited word at a
// time, including the unsplit branch. This is what nextTextPart has to reproduce.
static std::vector<std::string> pageParts(const char* text) {
  std::vector<std::string> out;
  const std::string s(text);
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') i++;
    size_t e = i;
    while (e < s.size() && s[e] != ' ') e++;
    if (e == i) break;
    const char* w = s.data() + i;
    const size_t wlen = e - i;
    bool isCjk = false;
    if (PageTokens::isSelectable(w, wlen, isCjk)) {
      PageTokens::Part parts[PageTokens::kMaxTokenParts];
      const size_t n = PageTokens::collectParts(w, wlen, parts, PageTokens::kMaxTokenParts);
      const bool unsplit = n == 1 && parts[0].start == 0 && parts[0].end == wlen;
      for (size_t p = 0; p < n; p++) {
        out.emplace_back(unsplit ? w : w + parts[p].start, unsplit ? wlen : parts[p].end - parts[p].start);
      }
    }
    i = e;
  }
  return out;
}

static std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) {
    if (!out.empty()) out += "|";
    out += s;
  }
  return out;
}

static void testTextPartsSkipUnselectableWords() {
  std::printf("nextTextPart skips a word carrying no letter or digit\n");
  // The defect this exists for: an en-dash standing between two words is a snippet
  // "word" with no page token behind it, and matching on spaces alone stalled there.
  CHECK(join(textParts("mkdir \xE2\x80\x93 Create Directories")) == "mkdir|Create|Directories",
        "a standalone en-dash is skipped");
  CHECK(join(textParts("one -- two")) == "one|two", "a standalone double hyphen is skipped");
  CHECK(join(textParts("... !? one")) == "one", "pure punctuation is skipped");
}

static void testTextPartsSplitOnDashes() {
  std::printf("nextTextPart splits a word on its dashes and drops the separator\n");
  CHECK(join(textParts("east--west")) == "east|west", "ascii double hyphen splits");
  CHECK(join(textParts("a\xE2\x80\x94"
                       "b c")) == "a|b|c",
        "em-dash splits");
  CHECK(join(textParts("trailing--")) == "trailing", "a trailing separator yields no part");
  CHECK(join(textParts("--leading")) == "leading", "a leading separator yields no part");
}

static void testTextPartsKeepEveryOtherMark() {
  std::printf("nextTextPart keeps a single hyphen and every other mark inside its part\n");
  CHECK(join(textParts("well-known")) == "well-known", "a single hyphen does not split");
  CHECK(join(textParts("(parenthesised)")) == "(parenthesised)", "brackets stay in the part");
  CHECK(join(textParts("don't \"quoted,\"")) == "don't|\"quoted,\"", "quotes and commas stay in the part");
}

static void testTextPartsExhaust() {
  std::printf("nextTextPart yields nothing when there is nothing to index\n");
  CHECK(textParts("").empty(), "empty string");
  CHECK(textParts("  ").empty(), "whitespace only");
  CHECK(textParts("\xE2\x80\x93 -- ...").empty(), "separators and punctuation only");
}

static void testTextPartsAgreeWithThePageWalk() {
  std::printf("nextTextPart agrees with the page walk on every shape\n");
  static const char* kCases[] = {"mkdir \xE2\x80\x93 Create Directories",
                                 "east--west and well-known",
                                 "don't stop",
                                 "a -- b",
                                 "trailing-- --leading",
                                 "(one) [two] {three}",
                                 "1996 - 2024",
                                 "one",
                                 "\xE4\xB8\xAD\xE6\x96\x87 mixed"};
  for (const char* text : kCases) {
    CHECK(join(textParts(text)) == join(pageParts(text)), text);
  }
}

// ---------------------------------------------------------------------------
// SnippetMatch: finding a stored quote among a page's words.
// ---------------------------------------------------------------------------

// Feed a snippet the page's words one at a time, as PageMarks does. Returns the word
// index the match opened at, or -1 if it never completed.
static int runMatch(const char* snippet, const std::vector<std::string>& words, const size_t cap) {
  SnippetMatch::Matcher m;
  m.snippet = snippet;
  m.begin(cap);
  uint16_t index = 0;
  for (const auto& w : words) {
    bool cjk = false;
    if (!PageTokens::isSelectable(w.c_str(), w.size(), cjk)) continue;
    PageTokens::Part parts[PageTokens::kMaxTokenParts];
    const size_t partCount = PageTokens::collectParts(w.c_str(), w.size(), parts, PageTokens::kMaxTokenParts);
    const bool unsplit = partCount == 1 && parts[0].start == 0 && parts[0].end == w.size();
    for (size_t pi = 0; pi < partCount; pi++, index++) {
      const size_t ps = unsplit ? 0 : parts[pi].start;
      const size_t pl = unsplit ? w.size() : parts[pi].end - parts[pi].start;
      if (m.offer(w.c_str() + ps, pl, index)) return static_cast<int>(m.start);
    }
  }
  return -1;
}

static void testWholeSnippetMatchesItsWords() {
  std::printf("a snippet matches the page words it names\n");
  const std::vector<std::string> page = {"the", "quick", "brown", "fox", "jumps"};
  CHECK(runMatch("quick brown fox", page, 64) == 1, "interior run matches at its first word");
  CHECK(runMatch("the quick", page, 64) == 0, "run at the page start matches at 0");
}

static void testSnippetAbsentFromThePageDoesNotMatch() {
  std::printf("a snippet not on the page does not match\n");
  const std::vector<std::string> page = {"the", "quick", "brown", "fox"};
  CHECK(runMatch("lazy dog", page, 64) == -1, "absent text does not match");
  // Present as words, but not as a run.
  CHECK(runMatch("quick fox", page, 64) == -1, "words present but not adjacent do not match");
}

// The device caps a snippet at its buffer and cuts wherever the cut falls, so a
// highlight longer than the cap ends mid-word. Requiring the page's word to fit inside
// that fragment failed EVERY such highlight: the mark landed on the right page and
// simply never drew.
static void testSnippetCutMidWordStillMatches() {
  std::printf("a snippet cut mid-word by the cap still matches\n");
  const std::vector<std::string> page = {"a", "playground,", "so", "let's", "play"};
  // cap 16 => "playground, so l" fills the buffer: the tail is the head of "let's".
  CHECK(runMatch("playground, so l", page, 17) == 1, "a snippet cut mid-word still matches");
}

// The relaxation is scoped to a snippet that actually reached the cap. A short one was
// never cut, so its last word is whole and a page word that merely starts with it is a
// different word.
static void testShortSnippetTailMustBeAWholeWord() {
  std::printf("an uncut snippet must match its tail whole\n");
  const std::vector<std::string> page = {"so", "let's", "play"};
  CHECK(runMatch("so l", page, 64) == -1, "an uncut snippet's tail must be a whole word");
  CHECK(runMatch("so let's", page, 64) == 0, "the same snippet matches when the tail is whole");
}

// Only the LAST part may be a fragment; a cut cannot shorten anything before it.
static void testInteriorWordMustStillMatchWhole() {
  std::printf("only the final part of a snippet may be a fragment\n");
  const std::vector<std::string> page = {"so", "let's", "play", "now"};
  CHECK(runMatch("so l play", page, 10) == -1, "only the last part may be a fragment");
}

static void testHyphenatedPageWordRejoins() {
  std::printf("a page word hyphenated across lines rejoins\n");
  // The page splits a hyphenated word across lines; the snippet stores it merged.
  const std::vector<std::string> page = {"a", "play-", "ground", "here"};
  CHECK(runMatch("playground here", page, 64) == 1, "a hyphenated page word rejoins");
}

// The real failing case from the X4 Pro log, at the real cap.
static void testTheLoggedFailureNowMatches() {
  std::printf("the quote the X4 Pro log reported unmatched now matches\n");
  const std::vector<std::string> page = {"passwd", "doesn't", "seem", "very",        "playful", "and",
                                         "this",   "is",      "a",    "playground,", "so",      "let's",
                                         "play",   "with",    "some", "of",          "its",     "options."};
  const char* snippet = "passwd doesn't seem very playful and this is a playground, so l";
  CHECK(std::strlen(snippet) == 63u, "the logged snippet is exactly the cap");
  CHECK(runMatch(snippet, page, 64) == 0, "the X4 Pro log's unmatched quote now matches");
}

// ---------------------------------------------------------------------------
// QuoteSpan: which part of a quote each page holds.
// ---------------------------------------------------------------------------

// A 4-page chapter: slice 0.25, pages at 0.00 / 0.25 / 0.50 / 0.75.
static QuoteSpan::Role roleOn(const int page, const float start, const float end) {
  const float slice = 0.25f;
  return QuoteSpan::pageRole(start, end, static_cast<float>(page) * slice, slice);
}

static void testAQuoteOnOnePageIsWhole() {
  std::printf("a quote that starts and ends on one page is Whole there and nowhere else\n");
  // A mark made on this device: end == start, always.
  CHECK(roleOn(1, 0.25f, 0.25f) == QuoteSpan::Role::Whole, "its own page");
  CHECK(roleOn(0, 0.25f, 0.25f) == QuoteSpan::Role::NotHere, "the page before");
  CHECK(roleOn(2, 0.25f, 0.25f) == QuoteSpan::Role::NotHere, "the page after");
}

static void testAQuoteAcrossTwoPagesSplits() {
  std::printf("a quote crossing one break starts on the first page and ends on the second\n");
  CHECK(roleOn(1, 0.25f, 0.50f) == QuoteSpan::Role::Starts, "the page it opens on");
  CHECK(roleOn(2, 0.25f, 0.50f) == QuoteSpan::Role::Ends, "the page it closes on");
  CHECK(roleOn(0, 0.25f, 0.50f) == QuoteSpan::Role::NotHere, "before it");
  CHECK(roleOn(3, 0.25f, 0.50f) == QuoteSpan::Role::NotHere, "after it");
}

static void testAQuoteAcrossThreePagesFillsTheMiddle() {
  std::printf("a quote spanning three pages highlights the middle one end to end\n");
  CHECK(roleOn(1, 0.25f, 0.75f) == QuoteSpan::Role::Starts, "first");
  CHECK(roleOn(2, 0.25f, 0.75f) == QuoteSpan::Role::Through, "middle");
  CHECK(roleOn(3, 0.25f, 0.75f) == QuoteSpan::Role::Ends, "last");
}

static void testTheLastPageOfAChapterIsReachable() {
  std::printf("a quote on the chapter's final page is found there\n");
  // pageStart + slice is exactly 1.0 there, so a test written as `< pageEnd` must not
  // exclude an end sitting at the last page's own fraction.
  CHECK(roleOn(3, 0.75f, 0.75f) == QuoteSpan::Role::Whole, "final page");
  CHECK(roleOn(3, 0.50f, 0.75f) == QuoteSpan::Role::Ends, "ending on the final page");
}

static void testAnInvertedEndIsTreatedAsNoSpan() {
  std::printf("an end before the start collapses to a single page\n");
  CHECK(roleOn(2, 0.50f, 0.25f) == QuoteSpan::Role::Whole, "its start page");
  CHECK(roleOn(1, 0.50f, 0.25f) == QuoteSpan::Role::NotHere, "not the bogus end page");
}

static void testTheEndFractionAndStopIndex() {
  std::printf("the end position within a page interpolates over its tokens\n");
  // A quote ending a quarter of the way into page 2 (0.50 .. 0.75).
  const uint8_t q = QuoteSpan::endFraction(0.5625f, 0.50f, 0.25f);
  CHECK(q > 60 && q < 70, "a quarter of a page is ~64/255");
  CHECK(QuoteSpan::stopIndex(q, 99) == 24 || QuoteSpan::stopIndex(q, 99) == 25, "~a quarter of 100 tokens");
  // The ends must be exact, not interpolated: a full page keeps its last token.
  CHECK(QuoteSpan::stopIndex(255, 99) == 99, "a full page stops at its last token");
  CHECK(QuoteSpan::stopIndex(0, 99) == 0, "a zero fraction stops at the first token");
  CHECK(QuoteSpan::stopIndex(255, 0) == 0, "a one-token page");
  CHECK(QuoteSpan::endFraction(0.40f, 0.50f, 0.25f) == 0, "an end before the page clamps to 0");
  CHECK(QuoteSpan::endFraction(0.90f, 0.50f, 0.25f) == 255, "an end past the page clamps to full");
}

int main() {
  testPartsMatchOriginalSplit();
  testUnsplitTokenIsWholeToken();
  testAsciiDoubleHyphenSplits();
  testSelectablePredicate();
  testMeasureStripsSoftHyphen();
  testTextPartsSkipUnselectableWords();
  testTextPartsSplitOnDashes();
  testTextPartsKeepEveryOtherMark();
  testTextPartsExhaust();
  testTextPartsAgreeWithThePageWalk();
  testWholeSnippetMatchesItsWords();
  testSnippetAbsentFromThePageDoesNotMatch();
  testSnippetCutMidWordStillMatches();
  testShortSnippetTailMustBeAWholeWord();
  testInteriorWordMustStillMatchWhole();
  testHyphenatedPageWordRejoins();
  testTheLoggedFailureNowMatches();
  testAQuoteOnOnePageIsWhole();
  testAQuoteAcrossTwoPagesSplits();
  testAQuoteAcrossThreePagesFillsTheMiddle();
  testTheLastPageOfAChapterIsReachable();
  testAnInvertedEndIsTreatedAsNoSpan();
  testTheEndFractionAndStopIndex();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
