#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "IndexTerm.h"
#include "SnippetMatch.h"

namespace {

// Lines separated by '|', tokens by spaces: "Bob Hawke , 323|Dan Quayle , 323".
struct Tokens {
  std::vector<std::string> storage;
  std::vector<IndexTerm::Token> tokens;

  explicit Tokens(const std::string& page) {
    uint16_t line = 0;
    std::string cur;
    std::vector<std::pair<std::string, uint16_t>> words;
    for (const char c : page + " ") {
      if (c == ' ' || c == '|') {
        if (!cur.empty()) words.push_back({cur, line});
        cur.clear();
        if (c == '|') line++;
      } else {
        cur += c;
      }
    }
    storage.reserve(words.size());
    for (const auto& [w, l] : words) storage.push_back(w);
    for (size_t i = 0; i < words.size(); i++) {
      tokens.push_back({storage[i].c_str(), static_cast<uint16_t>(storage[i].size()), words[i].second});
    }
  }

  // Index of the n-th token equal to `text` (0-based occurrence).
  size_t find(const char* text, int occurrence = 0) const {
    for (size_t i = 0; i < storage.size(); i++) {
      if (storage[i] == text && occurrence-- == 0) return i;
    }
    return SIZE_MAX;
  }

  std::string term(const size_t link) const {
    char out[IndexTerm::kMaxTerm];
    if (!IndexTerm::extractTerm(tokens.data(), tokens.size(), link, out, sizeof(out))) return "<none>";
    return out;
  }
};

std::vector<std::string> candidates(const char* term) {
  char out[IndexTerm::kMaxCandidates][IndexTerm::kMaxTerm];
  const size_t n = IndexTerm::buildCandidates(term, out, IndexTerm::kMaxCandidates);
  return {out, out + n};
}

}  // namespace

TEST(IndexTermPageRef, RecognisesNumbersNumeralsAndRanges) {
  EXPECT_TRUE(IndexTerm::isPageRef("323", 3));
  EXPECT_TRUE(IndexTerm::isPageRef("xvii", 4));
  EXPECT_TRUE(IndexTerm::isPageRef("336-339", 7));
  EXPECT_TRUE(IndexTerm::isPageRef("336\xE2\x80\x93" "339", 9));  // en dash
  EXPECT_FALSE(IndexTerm::isPageRef("Hawke", 5));
  EXPECT_FALSE(IndexTerm::isPageRef("XVII", 4));  // capitals are a word, not a numeral
  EXPECT_FALSE(IndexTerm::isPageRef("COVID-19", 8));
}

TEST(IndexTermPageRef, SeparatorsHaveNoLetterOrDigit) {
  EXPECT_TRUE(IndexTerm::isSeparator(",", 1));
  EXPECT_TRUE(IndexTerm::isSeparator("\xE2\x80\x93", 3));
  EXPECT_FALSE(IndexTerm::isSeparator("Chr\xC3\xA9tien", 9));
}

// The layout without focus reading: punctuation stays on its word, as these lines of the
// "Standing Tall" index lay out on the device ("Badawi," is one word, the link "207" another).
TEST(IndexTermExtract, PunctuationAttachedToWords) {
  Tokens t("Abdullah Badawi, 207 , 322|African National Congress (ANC), 143 , 144|Al-Qaeda, 231 , 232|"
           "Bob Hawke, 323|Chiam See Tong, 17 , 18 , 42 ,|119 , 224");
  EXPECT_EQ(t.term(t.find("207")), "Abdullah Badawi");
  EXPECT_EQ(t.term(t.find("322")), "Abdullah Badawi");
  EXPECT_EQ(t.term(t.find("144")), "African National Congress (ANC)");
  EXPECT_EQ(t.term(t.find("232")), "Al-Qaeda");
  EXPECT_EQ(t.term(t.find("323")), "Bob Hawke");
  EXPECT_EQ(t.term(t.find("224")), "Chiam See Tong");
}

TEST(IndexTermExtract, AttachedCommaOnReferencesAndInvertedNames) {
  Tokens t("Hawke, Bob, 323, 325|Edusave, xvii, 56");
  EXPECT_EQ(t.term(t.find("325")), "Hawke, Bob");
  EXPECT_EQ(t.term(t.find("56")), "Edusave");
}

TEST(IndexTermExtract, NoteNumbersHaveNoTermWithAttachedPunctuation) {
  Tokens notes("Marshall Cavendish Editions, 2019), p. 51.|2 The questions mean");
  EXPECT_EQ(notes.term(notes.find("2")), "<none>");
  Tokens prose("most toxic in recent Singapore's history. 1 Goh's appreciation");
  EXPECT_EQ(prose.term(prose.find("1")), "<none>");
}

// Focus reading splits punctuation into words of its own: "Hawke" ",".
TEST(IndexTermExtract, SimpleEntry) {
  Tokens t("Bill Clinton , 322|Bob Hawke , 323|Dan Quayle , 323");
  EXPECT_EQ(t.term(t.find("323", 0)), "Bob Hawke");
  EXPECT_EQ(t.term(t.find("323", 1)), "Dan Quayle");
}

TEST(IndexTermExtract, LaterReferenceOfLongEntryOnWrappedLine) {
  Tokens t("Cheo Chai Chen , 108|Chiam See Tong , 17 , 18 , 42 , 118 ,|119 , 224 , 225|Chinese Communist Party");
  EXPECT_EQ(t.term(t.find("224")), "Chiam See Tong");
}

TEST(IndexTermExtract, LetterHeadingIsNotPartOfTheTerm) {
  Tokens t("Crystal Lim , 55 , 60|D|Dan Quayle , 323");
  EXPECT_EQ(t.term(t.find("323")), "Dan Quayle");
}

TEST(IndexTermExtract, RomanNumeralsAndRanges) {
  Tokens t("Flor Contemplacion , xix|Cherian George , xvii , 40|COVID - 19 , vii , 22 , 336-339");
  EXPECT_EQ(t.term(t.find("40")), "Cherian George");
  EXPECT_EQ(t.term(t.find("xvii")), "Cherian George");
  EXPECT_EQ(t.term(t.find("336-339")), "COVID");  // "-19" reads as references: a known limit
}

TEST(IndexTermExtract, TermWrappedOntoTwoLines) {
  Tokens t("Comprehensive Economic Cooperation|Agreement with India , 261");
  EXPECT_EQ(t.term(t.find("261")), "Comprehensive Economic Cooperation Agreement with India");
}

TEST(IndexTermExtract, HyphenatedLineBreakIsRejoined) {
  Tokens t("Corrupt Practices Investi-|gation Bureau ( CPIB ) , 306");
  // The ")" before the comma is taken with the references; candidates still read "CPIB".
  EXPECT_EQ(t.term(t.find("306")), "Corrupt Practices Investigation Bureau ( CPIB");
  EXPECT_EQ(candidates("Corrupt Practices Investigation Bureau ( CPIB"),
            (std::vector<std::string>{"Corrupt Practices Investigation Bureau", "CPIB", "Corrupt Practices", "Bureau"}));
}

TEST(IndexTermExtract, FootnoteMarkersAndNoteNumbersHaveNoTerm) {
  Tokens prose("most toxic in recent Singapore's history . 1 Goh's appreciation");
  EXPECT_EQ(prose.term(prose.find("1")), "<none>");
  Tokens notes("Marshall Cavendish Editions , 2019 ) , p . 51 .|2 The questions mean");
  EXPECT_EQ(notes.term(notes.find("2")), "<none>");
  Tokens word("Bob Hawke , 323");
  EXPECT_EQ(word.term(word.find("Hawke")), "<none>");  // not a page reference
}

TEST(IndexTermCandidates, FullNameThenSurname) {
  EXPECT_EQ(candidates("Bob Hawke"), (std::vector<std::string>{"Bob Hawke", "Hawke"}));
}

TEST(IndexTermCandidates, ParentheticalBecomesItsOwnCandidate) {
  EXPECT_EQ(candidates("Corrupt Practices Investigation Bureau ( CPIB )"),
            (std::vector<std::string>{"Corrupt Practices Investigation Bureau", "CPIB", "Corrupt Practices", "Bureau"}));
}

TEST(IndexTermCandidates, AttachedPunctuation) {
  EXPECT_EQ(candidates("Hawke, Bob"), (std::vector<std::string>{"Bob Hawke", "Hawke"}));
  EXPECT_EQ(candidates("African National Congress (ANC)"),
            (std::vector<std::string>{"African National Congress", "ANC", "African National", "Congress"}));
  EXPECT_EQ(candidates("Corrupt Practices Investigation Bureau (CPIB)"),
            (std::vector<std::string>{"Corrupt Practices Investigation Bureau", "CPIB", "Corrupt Practices", "Bureau"}));
  EXPECT_EQ(candidates("Chinese Communist Party (Chinese Communists)"),
            (std::vector<std::string>{"Chinese Communist Party", "Chinese Communists", "Chinese Communist", "Party"}));
}

TEST(IndexTermCandidates, InvertedNameIsReordered) {
  EXPECT_EQ(candidates("Hawke , Bob"), (std::vector<std::string>{"Bob Hawke", "Hawke"}));
}

TEST(IndexTermCandidates, LeadingJoiningWordsAndShortStringsAreDropped) {
  EXPECT_EQ(candidates("and Lee Kuan Yew"), (std::vector<std::string>{"Lee Kuan Yew", "Lee Kuan"}));
  EXPECT_EQ(candidates("EU"), (std::vector<std::string>{"EU"}));  // capitals: an abbreviation
  EXPECT_TRUE(candidates("Ox").empty());
  EXPECT_EQ(candidates("Cooling - Off Day"), (std::vector<std::string>{"Cooling Off Day", "Cooling Off"}));
}

TEST(IndexTermCandidates, LooksLikeIndex) {
  EXPECT_TRUE(IndexTerm::looksLikeIndex(19, 19));
  EXPECT_TRUE(IndexTerm::looksLikeIndex(10, 6));
  EXPECT_FALSE(IndexTerm::looksLikeIndex(5, 5));
  EXPECT_FALSE(IndexTerm::looksLikeIndex(10, 5));
}

// The index search matches case-insensitively; marks keep exact matching.
TEST(SnippetMatchCaseFold, FoldsAsciiOnlyWhenAsked) {
  const auto matches = [](const char* snippet, const std::vector<const char*>& page, const bool fold,
                          uint16_t* end = nullptr) {
    SnippetMatch::Matcher m;
    m.snippet = snippet;
    m.caseFold = fold;
    m.begin(64);
    for (uint16_t i = 0; i < page.size(); i++) {
      if (m.offer(page[i], strlen(page[i]), i)) {
        if (end) *end = m.end;
        return true;
      }
    }
    return false;
  };
  EXPECT_FALSE(matches("Cooling Off Day", {"the", "cooling", "off", "day"}, false));
  uint16_t end = 0;
  EXPECT_TRUE(matches("Cooling Off Day", {"the", "cooling", "off", "day", "was"}, true, &end));
  EXPECT_EQ(end, 3);
}

// Page words keep their punctuation ("Hawke,"); a searched name matches the word inside it.
TEST(SnippetMatchCaseFold, PageWordPunctuationIsIgnoredWhenSearching) {
  const auto matches = [](const char* snippet, const std::vector<const char*>& page, const bool fold) {
    SnippetMatch::Matcher m;
    m.snippet = snippet;
    m.caseFold = fold;
    m.begin(64);
    for (uint16_t i = 0; i < page.size(); i++) {
      if (m.offer(page[i], strlen(page[i]), i)) return true;
    }
    return false;
  };
  EXPECT_TRUE(matches("Bob Hawke", {"Minister", "Bob", "Hawke,", "when"}, true));
  EXPECT_TRUE(matches("Hawke", {"(Hawke"}, true));
  EXPECT_TRUE(matches("Hawke", {"Hawke's"}, true));
  EXPECT_TRUE(matches("Hawke", {"Hawke\xE2\x80\x99s"}, true));
  EXPECT_TRUE(matches("Mahathir Mohamad", {"\xE2\x80\x9CMahathir", "Mohamad.\xE2\x80\x9D"}, true));
  EXPECT_FALSE(matches("Hawke", {"Hawkesbury"}, true));  // a longer word is not the name
  EXPECT_FALSE(matches("Bob Hawke", {"Bob", "Hawke,"}, false));  // quotes keep exact matching
}

TEST(IndexTermPageRef, RangeSplitAtItsDash) {
  EXPECT_TRUE(IndexTerm::isPageRef("-47", 3));
  EXPECT_TRUE(IndexTerm::isPageRef("–47", 5));
  EXPECT_TRUE(IndexTerm::isPageRef("146-", 4));
  EXPECT_FALSE(IndexTerm::isPageRef("-", 1));
  EXPECT_FALSE(IndexTerm::isPageRef("civil-", 6));
  EXPECT_TRUE(IndexTerm::isPageRefLabel("146 -47"));
  EXPECT_TRUE(IndexTerm::isPageRefLabel("307 -8,"));
  EXPECT_TRUE(IndexTerm::isPageRefLabel("186 ,"));
  EXPECT_FALSE(IndexTerm::isPageRefLabel("Hawke"));
  EXPECT_FALSE(IndexTerm::isPageRefLabel(""));
}

TEST(IndexTermExtract, SplitRangeClosesThePreviousEntry) {
  const Tokens t("Alps Electronics Co., 146 -47|Altair (personal computer), 59");
  EXPECT_EQ(t.term(t.find("59")), "Altair (personal computer)");
  EXPECT_EQ(t.term(t.find("146")), "Alps Electronics Co");
  const Tokens u("Anderson, Abe, 307 -8 Anderson, Fred, 313");
  EXPECT_EQ(u.term(u.find("313")), "Anderson, Fred");
  const Tokens v("Apple-NeXT deal and, 299 -303|Macworld gaffe of, 307 -8, 311");
  EXPECT_EQ(v.term(v.find("307")), "Macworld gaffe of");
}

TEST(IndexTermCandidates, QuotedTitle) {
  EXPECT_EQ(candidates("\"All Watched Over by Machines of Loving Grace\" (Brautigan)"),
            (std::vector<std::string>{"All Watched Over by Machines of Loving Grace", "Brautigan", "All Watched",
                                      "Grace"}));
  EXPECT_EQ(candidates("“Think Different” campaign"),
            (std::vector<std::string>{"Think Different campaign", "Think Different"}));
}

TEST(IndexTermCandidates, SubEntryDropsJoiningWordAndFallsBackToLongestWord) {
  EXPECT_EQ(candidates("media and"), (std::vector<std::string>{"media"}));
  EXPECT_EQ(candidates("Macworld gaffe of"), (std::vector<std::string>{"Macworld gaffe", "Macworld"}));
  EXPECT_EQ(candidates("Apple-NeXT deal and"), (std::vector<std::string>{"Apple-NeXT deal", "Apple-NeXT"}));
  EXPECT_EQ(candidates("early competition to"), (std::vector<std::string>{"early competition"}));
  EXPECT_EQ(candidates("stock options controversy and"),
            (std::vector<std::string>{"stock options controversy", "stock options"}));
}

TEST(IndexTermCandidates, CommonWordIsNeverACandidateAlone) {
  EXPECT_EQ(candidates("Apple I computer"), (std::vector<std::string>{"Apple I computer", "Apple I"}));
  EXPECT_EQ(candidates("Apple II computer"), (std::vector<std::string>{"Apple II computer", "Apple II"}));
  EXPECT_EQ(candidates("Bank of America"), (std::vector<std::string>{"Bank of America", "America"}));
  EXPECT_EQ(candidates("Apple Stores"), (std::vector<std::string>{"Apple Stores", "Apple Store", "Stores"}));
}

TEST(IndexTermExtract, CrossReferenceLineEndsTheTerm) {
  const Tokens t("Apple Marketing Philosophy, 78|Apple products, see individual product names|Apple Stores, 368");
  EXPECT_EQ(t.term(t.find("368")), "Apple Stores");
  const Tokens u("Apple products (see individual product names)|Apple Stores, 368");
  EXPECT_EQ(u.term(u.find("368")), "Apple Stores");
  const Tokens w("Apple Marketing Philosophy, 78|Apple products and the|Stores, 368");
  EXPECT_EQ(w.term(w.find("368")), "Apple products and the Stores");
}

TEST(IndexTermCandidates, ThreeLetterSurnameOfInvertedName) {
  EXPECT_EQ(candidates("Ali, Muhammad"), (std::vector<std::string>{"Muhammad Ali", "Ali"}));
  EXPECT_EQ(candidates("ANC"), (std::vector<std::string>{"ANC"}));
  EXPECT_EQ(candidates("art"), (std::vector<std::string>{}));
}

TEST(IndexTermExtract, CrossReferenceWrappedOverLines) {
  const Tokens t("Apple Marketing Philosophy, 78|Apple products, see individual|product names|Apple Stores, 368 -77,");
  EXPECT_EQ(t.term(t.find("368")), "Apple Stores");
}

namespace {
// Tokens with an indent per line: "x:words|x:words".
struct Indented : Tokens {
  explicit Indented(const std::string& page, const std::vector<int16_t>& indents) : Tokens(page) {
    for (auto& t : tokens) t.x = indents[t.line];
  }
  std::string parent(const size_t link) const {
    char term[IndexTerm::kMaxTerm];
    size_t first = 0;
    if (!IndexTerm::extractTerm(tokens.data(), tokens.size(), link, term, sizeof(term), &first)) return "<none>";
    char out[IndexTerm::kMaxTerm];
    if (!IndexTerm::extractParent(tokens.data(), tokens.size(), first, out, sizeof(out))) return "<none>";
    return out;
  }
};
}  // namespace

TEST(IndexTermParent, IndentedSubEntryTakesItsHeading) {
  const Indented t("Apple III computer, 92 -94, 105|failure of, 92 -93, 160|launch of, 99|Apple Stores, 368",
                   {0, 20, 20, 0});
  EXPECT_EQ(t.parent(t.find("160")), "Apple III computer");
  EXPECT_EQ(t.parent(t.find("99")), "Apple III computer");
  EXPECT_EQ(t.parent(t.find("368")), "<none>");  // a heading of its own
  EXPECT_EQ(t.term(t.find("105")), "Apple III computer");
  EXPECT_EQ(t.parent(t.find("105")), "<none>");
}

TEST(IndexTermParent, RunInSubEntryTakesTheLineItContinuesFrom) {
  const Indented t("Apple III computer, 92 -94, 105; failure of,|92 -93, 160; launch of, 99", {0, 20});
  EXPECT_EQ(t.term(t.find("160;")), "failure of");
  EXPECT_EQ(t.parent(t.find("160;")), "Apple III computer");
  EXPECT_EQ(t.parent(t.find("99")), "Apple III computer");
  EXPECT_EQ(t.term(t.find("105;")), "Apple III computer");
  EXPECT_EQ(t.parent(t.find("105;")), "<none>");
}

TEST(IndexTermParent, FlushIndexHasNoHeadings) {
  const Indented t("Apple II computer, 91|Apple III computer, 92", {0, 0});
  EXPECT_EQ(t.parent(t.find("92")), "<none>");
}

TEST(IndexTermCandidates, SubEntryFallsBackToItsHeading) {
  EXPECT_EQ(candidates("failure of\x1F" "Apple III computer"),
            (std::vector<std::string>{"failure", "Apple III computer", "Apple III"}));
  EXPECT_EQ(candidates("checkout design of\x1F" "Apple Stores"),
            (std::vector<std::string>{"checkout design", "checkout", "Apple Stores"}));
}

TEST(IndexTermParent, OneWordHeadingIsNotSearched) {
  const Indented t("Jobs, Steve:|adoption of, 3|Apple Corps, 419", {0, 20, 0});
  EXPECT_EQ(t.term(t.find("3")), "adoption of");
  EXPECT_EQ(t.parent(t.find("3")), "<none>");
}

TEST(IndexTermExtract, CommaInsideClosingQuote) {
  const Tokens t("Ansen, David, 290|\"Antennagate,\" 519 -23|Apple Corps, 419");
  EXPECT_EQ(t.term(t.find("519")), "\"Antennagate");
  EXPECT_EQ(candidates("\"Antennagate"), (std::vector<std::string>{"Antennagate"}));
  const Tokens u("Cunningham, Andrea \u201cAndy,\u201d xiii , 93");
  EXPECT_EQ(u.term(u.find("93")), "Cunningham, Andrea \u201cAndy");
}

TEST(IndexTermPageRef, RomanRangeTail) {
  EXPECT_TRUE(IndexTerm::isPageRef("-xviii", 6));
  EXPECT_TRUE(IndexTerm::isPageRefLabel("xvii -xviii,"));
  EXPECT_FALSE(IndexTerm::isPageRef("civil-", 6));
  const Tokens t("Apple Corps, 419|SJ ousted from, xvii -xviii, 202 -6, 215");
  EXPECT_EQ(t.term(t.find("202")), "SJ ousted from");
}

TEST(IndexTermCandidates, PluralPhraseAlsoSearchedSingular) {
  EXPECT_EQ(candidates("Academy Awards"), (std::vector<std::string>{"Academy Awards", "Academy Award", "Awards"}));
  EXPECT_EQ(candidates("Apple Corps"), (std::vector<std::string>{"Apple Corps", "Apple Corp", "Corps"}));
  EXPECT_EQ(candidates("Bob Hawke"), (std::vector<std::string>{"Bob Hawke", "Hawke"}));
}

TEST(IndexTermCandidates, SingleWordPluralAlsoSearchedSingular) {
  EXPECT_EQ(candidates("prototypes of\x1F" "Apple Stores"),
            (std::vector<std::string>{"prototypes", "prototype", "Apple Stores"}));
  EXPECT_EQ(candidates("status"), (std::vector<std::string>{"status"}));
  EXPECT_EQ(candidates("glass"), (std::vector<std::string>{"glass"}));
}

TEST(IndexTermCandidates, CategoryWordIsDropped) {
  EXPECT_EQ(candidates("chmod command"), (std::vector<std::string>{"chmod command", "chmod"}));
  EXPECT_EQ(candidates("cat command"), (std::vector<std::string>{"cat command", "cat"}));
  EXPECT_EQ(candidates("AWK programming language"),
            (std::vector<std::string>{"AWK programming language", "AWK", "AWK programming"}));
  EXPECT_EQ(candidates("PATH variable"), (std::vector<std::string>{"PATH variable", "PATH"}));
  EXPECT_EQ(candidates("command"), (std::vector<std::string>{"command"}));  // nothing left to name
}
