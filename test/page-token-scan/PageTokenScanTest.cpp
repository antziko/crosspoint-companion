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

int main() {
  testPartsMatchOriginalSplit();
  testUnsplitTokenIsWholeToken();
  testSelectablePredicate();
  testMeasureStripsSoftHyphen();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
