// Host-side tests for DictionaryRegistry::nextIndex — the cycle order behind the
// long-press-Confirm dictionary switch on the definition screen.
//
// Only the static, stateless half of DictionaryRegistry is exercised. The discovery
// half is SD-backed and deliberately not linked here; nextIndex was split out as a
// static precisely so this needs no HalStorage stub.

#include <gtest/gtest.h>

#include "util/DictionaryRegistry.h"

// The ordinary case: each press advances one, and the last wraps to the first.
TEST(DictRegistryCycle, AdvancesAndWraps) {
  EXPECT_EQ(DictionaryRegistry::nextIndex(0, 3), 1);
  EXPECT_EQ(DictionaryRegistry::nextIndex(1, 3), 2);
  EXPECT_EQ(DictionaryRegistry::nextIndex(2, 3), 0);
}

// indexOf() returns -1 when the configured dictionary isn't among the installed
// entries (e.g. it was deleted off the card). The cycle must still start somewhere
// rather than propagating the -1 into an out-of-bounds entries_ access.
TEST(DictRegistryCycle, OutOfRangeCurrentStartsAtZero) {
  EXPECT_EQ(DictionaryRegistry::nextIndex(-1, 3), 0);
  EXPECT_EQ(DictionaryRegistry::nextIndex(3, 3), 0);
  EXPECT_EQ(DictionaryRegistry::nextIndex(99, 3), 0);
  EXPECT_EQ(DictionaryRegistry::nextIndex(-42, 3), 0);
}

// A single installed dictionary cycles to itself. The caller additionally gates the
// gesture on count() > 1, so this is the defensive answer rather than the live path.
TEST(DictRegistryCycle, SingleEntryCyclesToItself) {
  EXPECT_EQ(DictionaryRegistry::nextIndex(0, 1), 0);
  EXPECT_EQ(DictionaryRegistry::nextIndex(-1, 1), 0);
}

// No dictionaries: -1 signals "nothing to switch to" and the caller bails out.
// A modulo-by-zero here would be a divide fault on the device.
TEST(DictRegistryCycle, EmptyListReturnsNegativeOne) {
  EXPECT_EQ(DictionaryRegistry::nextIndex(0, 0), -1);
  EXPECT_EQ(DictionaryRegistry::nextIndex(-1, 0), -1);
  EXPECT_EQ(DictionaryRegistry::nextIndex(5, -3), -1);
}

// Cycling the full length returns to where it started, for any starting point.
TEST(DictRegistryCycle, FullCycleReturnsToStart) {
  constexpr int kCount = 4;
  for (int start = 0; start < kCount; ++start) {
    int idx = start;
    for (int i = 0; i < kCount; ++i) idx = DictionaryRegistry::nextIndex(idx, kCount);
    EXPECT_EQ(idx, start) << "starting from " << start;
  }
}

// --- Group-partitioned cycling ------------------------------------------------
//
// nextIndexInGroup keeps the long-press switch inside one family of dictionaries:
// a type-'m' dictionary only ever cycles to another type-'m' one, and a non-'m'
// only to another non-'m'. The group flag is DictionaryEntry::typeIsM, derived
// from the StarDict sametypesequence at discovery time.
//
// The production caller reads that flag off entries_; here it comes from a plain
// array through the same accessor, so these exercise the shipped scan rather than
// a copy of it.

namespace {
// Captureless, so it converts to the plain function pointer nextIndexInGroup takes.
bool groupFromArray(const void* ctx, int index) { return static_cast<const bool*>(ctx)[index]; }

int nextInGroup(int current, const bool* isM, int count) {
  return DictionaryRegistry::nextIndexInGroup(current, count, groupFromArray, isM);
}
}  // namespace

// Interleaved m / non-m: each group skips over the other's members.
TEST(DictRegistryCycleGroup, SkipsTheOtherGroup) {
  //                    0     1      2     3
  const bool isM[] = {true, false, true, false};
  // m group is {0, 2}
  EXPECT_EQ(nextInGroup(0, isM, 4), 2);
  EXPECT_EQ(nextInGroup(2, isM, 4), 0);
  // non-m group is {1, 3}
  EXPECT_EQ(nextInGroup(1, isM, 4), 3);
  EXPECT_EQ(nextInGroup(3, isM, 4), 1);
}

// Groups need not be the same size, and the scan wraps past the end of the list.
TEST(DictRegistryCycleGroup, WrapsWithUnevenGroups) {
  //                    0      1      2     3      4
  const bool isM[] = {false, false, true, false, false};
  // Sole m entry has nobody to cycle to.
  EXPECT_EQ(nextInGroup(2, isM, 5), -1);
  // non-m group is {0, 1, 3, 4} and skips index 2.
  EXPECT_EQ(nextInGroup(1, isM, 5), 3);
  EXPECT_EQ(nextInGroup(4, isM, 5), 0);  // wraps
}

// The whole point of the change: a group of one must NOT cycle to itself, or the
// long press would re-run the identical lookup in the identical dictionary. -1
// makes handleLongPressDictSwitch's existing `< 0` guard leave the screen alone.
TEST(DictRegistryCycleGroup, SingleMemberGroupReturnsNegativeOne) {
  const bool oneOfEach[] = {true, false};
  EXPECT_EQ(nextInGroup(0, oneOfEach, 2), -1);
  EXPECT_EQ(nextInGroup(1, oneOfEach, 2), -1);

  const bool single[] = {true};
  EXPECT_EQ(nextInGroup(0, single, 1), -1);
}

// With every dictionary in the same group the partition is a no-op, so this must
// behave exactly like the plain nextIndex it replaces.
TEST(DictRegistryCycleGroup, AllSameGroupMatchesPlainNextIndex) {
  const bool allM[] = {true, true, true};
  const bool allOther[] = {false, false, false};
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(nextInGroup(i, allM, 3), DictionaryRegistry::nextIndex(i, 3));
    EXPECT_EQ(nextInGroup(i, allOther, 3), DictionaryRegistry::nextIndex(i, 3));
  }
}

// Same contract as nextIndex: an out-of-range current — including the -1 indexOf()
// returns when the configured dictionary is not among the installed entries — has
// no group to match, so it starts the cycle at 0 rather than refusing to switch.
TEST(DictRegistryCycleGroup, OutOfRangeCurrentStartsAtZero) {
  const bool isM[] = {true, false, true};
  EXPECT_EQ(nextInGroup(-1, isM, 3), 0);
  EXPECT_EQ(nextInGroup(3, isM, 3), 0);
  EXPECT_EQ(nextInGroup(99, isM, 3), 0);
}

// Degenerate inputs must not divide by zero or dereference the flag array.
TEST(DictRegistryCycleGroup, EmptyListReturnsNegativeOne) {
  const bool isM[] = {true};
  EXPECT_EQ(nextInGroup(0, isM, 0), -1);
  EXPECT_EQ(nextInGroup(-1, isM, 0), -1);
  EXPECT_EQ(nextInGroup(5, isM, -3), -1);
  EXPECT_EQ(DictionaryRegistry::nextIndexInGroup(0, 2, nullptr, nullptr), -1);
}

// Repeated presses visit every member of the group and nothing else.
TEST(DictRegistryCycleGroup, FullCycleVisitsOnlyOwnGroup) {
  //                    0     1      2     3     4      5
  const bool isM[] = {true, false, true, true, false, false};
  int idx = 0;
  for (int i = 0; i < 3; ++i) {
    idx = nextInGroup(idx, isM, 6);
    ASSERT_GE(idx, 0);
    EXPECT_TRUE(isM[idx]) << "left the m group at step " << i;
  }
  EXPECT_EQ(idx, 0) << "three presses over a 3-member group should return to the start";
}
