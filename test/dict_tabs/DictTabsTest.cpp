// DictTabs::order: which Words / Names / Grammar tabs the definition screen offers.
#include <gtest/gtest.h>

#include "util/DictTabs.h"

using namespace DictTabs;

TEST(DictTabs, SlotFromTag) {
  EXPECT_EQ(slotFromTag("vocab"), kWords);
  EXPECT_EQ(slotFromTag("jmnedict"), kNames);
  EXPECT_EQ(slotFromTag("grammar"), kGrammar);
  EXPECT_EQ(slotFromTag(""), 0);
  EXPECT_EQ(slotFromTag("other"), 0);
}

TEST(DictTabs, WordsThenNamesWhenBothHit) {
  // Registry: 0 = an English dictionary, 3 = names, 5 = vocab.
  const uint8_t slots[] = {0, kNames, kWords};
  const int idx[] = {0, 3, 5};
  Tab out[kMaxTabs];
  ASSERT_EQ(order(slots, idx, 3, kWords | kNames, 5, out), 2);
  EXPECT_EQ(out[0].registryIdx, 5);
  EXPECT_EQ(out[1].registryIdx, 3);
}

TEST(DictTabs, OneHitShowsNoTabs) {
  const uint8_t slots[] = {kNames, kWords};
  const int idx[] = {3, 5};
  Tab out[kMaxTabs];
  EXPECT_EQ(order(slots, idx, 2, kWords, 5, out), 0);
}

TEST(DictTabs, UntaggedActiveShowsNoTabs) {
  const uint8_t slots[] = {0, kNames, kWords};
  const int idx[] = {0, 3, 5};
  Tab out[kMaxTabs];
  EXPECT_EQ(order(slots, idx, 3, kWords | kNames, 0, out), 0);
}

TEST(DictTabs, ActiveKeepsItsSlotWhenDuplicated) {
  // Two vocab copies: the active one is the Words tab.
  const uint8_t slots[] = {kWords, kWords, kNames};
  const int idx[] = {1, 2, 4};
  Tab out[kMaxTabs];
  ASSERT_EQ(order(slots, idx, 3, kWords | kNames, 2, out), 2);
  EXPECT_EQ(out[0].registryIdx, 2);
}
