#include "util/DictStopwords.h"

#include <gtest/gtest.h>

#include <cstring>

using DictStopwords::isStopword;
using DictStopwords::STOPWORD_COUNT;
using DictStopwords::STOPWORDS;

// The binary search is only correct if the table is sorted ascending (strcmp
// order) with no duplicates. Guard that invariant here so a future hand-edit of
// the list cannot silently break lookups.
TEST(DictStopwords, TableIsSortedAndUnique) {
  for (size_t i = 1; i < STOPWORD_COUNT; ++i) {
    EXPECT_LT(std::strcmp(STOPWORDS[i - 1], STOPWORDS[i]), 0)
        << "out of order or duplicate at index " << i << ": '" << STOPWORDS[i - 1] << "' vs '" << STOPWORDS[i] << "'";
  }
}

// Every entry must be at most MAX_STOPWORD_LEN, otherwise the length early-out
// would skip it and it could never match.
TEST(DictStopwords, AllEntriesWithinLengthCap) {
  for (size_t i = 0; i < STOPWORD_COUNT; ++i) {
    EXPECT_LE(std::strlen(STOPWORDS[i]), DictStopwords::MAX_STOPWORD_LEN) << "too long: '" << STOPWORDS[i] << "'";
  }
}

// Every entry in the table must itself match (round-trip).
TEST(DictStopwords, EveryEntryMatches) {
  for (size_t i = 0; i < STOPWORD_COUNT; ++i) {
    EXPECT_TRUE(isStopword(std::string_view(STOPWORDS[i]))) << "entry did not match: '" << STOPWORDS[i] << "'";
  }
}

TEST(DictStopwords, KnownStopwordsMatch) {
  EXPECT_TRUE(isStopword(std::string_view("the")));
  EXPECT_TRUE(isStopword(std::string_view("a")));
  EXPECT_TRUE(isStopword(std::string_view("should")));
  EXPECT_TRUE(isStopword(std::string_view("would")));
  EXPECT_TRUE(isStopword(std::string_view("upon")));
  EXPECT_TRUE(isStopword(std::string_view("what")));
  EXPECT_TRUE(isStopword(std::string_view("on")));
  EXPECT_TRUE(isStopword(std::string_view("in")));
}

TEST(DictStopwords, ContentWordsDoNotMatch) {
  EXPECT_FALSE(isStopword(std::string_view("encyclopedia")));
  EXPECT_FALSE(isStopword(std::string_view("river")));
  EXPECT_FALSE(isStopword(std::string_view("running")));
  EXPECT_FALSE(isStopword(std::string_view("dictionary")));
  EXPECT_FALSE(isStopword(std::string_view("serendipity")));
}

// Surface-ambiguous words deliberately left OUT of the list (have a common
// content sense) must NOT be filtered.
TEST(DictStopwords, AmbiguousWordsDeliberatelyKept) {
  EXPECT_FALSE(isStopword(std::string_view("will")));
  EXPECT_FALSE(isStopword(std::string_view("can")));
  EXPECT_FALSE(isStopword(std::string_view("may")));
  EXPECT_FALSE(isStopword(std::string_view("might")));
  EXPECT_FALSE(isStopword(std::string_view("must")));
  EXPECT_FALSE(isStopword(std::string_view("well")));
  EXPECT_FALSE(isStopword(std::string_view("like")));
  EXPECT_FALSE(isStopword(std::string_view("back")));
  EXPECT_FALSE(isStopword(std::string_view("mine")));
}

TEST(DictStopwords, CaseInsensitive) {
  EXPECT_TRUE(isStopword(std::string_view("The")));
  EXPECT_TRUE(isStopword(std::string_view("SHOULD")));
  EXPECT_TRUE(isStopword(std::string_view("WhAt")));
}

TEST(DictStopwords, EmptyAndOverlengthEarlyOut) {
  EXPECT_FALSE(isStopword(std::string_view("")));
  EXPECT_FALSE(isStopword(nullptr, 0));
  // A token longer than the cap can never be a stopword (and must not read OOB).
  EXPECT_FALSE(isStopword(std::string_view("antidisestablishmentarianism")));
}

// Only the first `len` bytes are read — safe on a non-null-terminated substring.
TEST(DictStopwords, RespectsLengthNotNullTerminator) {
  const char* buf = "theory";       // "the" is a prefix
  EXPECT_TRUE(isStopword(buf, 3));   // "the"
  EXPECT_FALSE(isStopword(buf, 6));  // "theory"
}
