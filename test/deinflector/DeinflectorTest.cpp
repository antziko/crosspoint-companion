// Deinflector (verbatim from matcha-reader): conjugated forms reach their dictionary form, which
// word-select looks up when the surface form misses.
#include <gtest/gtest.h>

#include <string>

#include "Deinflector.h"

namespace {

bool hasCandidate(const char* surface, const char* expected) {
  for (const auto& c : Deinflector::deinflect(surface)) {
    if (c.text == expected) return true;
  }
  return false;
}

}  // namespace

TEST(Deinflector, GodanPast) { EXPECT_TRUE(hasCandidate("使った", "使う")); }

TEST(Deinflector, IchidanPolitePast) { EXPECT_TRUE(hasCandidate("食べました", "食べる")); }

TEST(Deinflector, AdjectivePast) { EXPECT_TRUE(hasCandidate("美しかった", "美しい")); }

TEST(Deinflector, SuruPassiveProgressive) { EXPECT_TRUE(hasCandidate("されていた", "する")); }

TEST(Deinflector, Negative) { EXPECT_TRUE(hasCandidate("読まない", "読む")); }

TEST(Deinflector, SurfaceComesFirst) {
  const auto c = Deinflector::deinflect("使った");
  ASSERT_FALSE(c.empty());
  EXPECT_EQ(c.front().text, "使った");
}

// The gloss box probes only the first few candidates (skipping the surface form).
TEST(Deinflector, IchidanNegativeIsAnEarlyCandidate) {
  int rank = 0;
  for (const auto& c : Deinflector::deinflect("認めない")) {
    if (c.text == "認めない") continue;
    if (c.text == "認める") break;
    rank++;
  }
  EXPECT_LT(rank, 6);
}
