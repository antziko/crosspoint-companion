#include "FileWindowSelect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <random>
#include <string>
#include <vector>

using filewindow::Entry;
using filewindow::WindowSelector;

namespace {

bool plainLess(const std::string& a, const std::string& b) { return a < b; }

// A deliberately tie-prone weak order (case-insensitive) wrapped with a byte tiebreak —
// mirrors how the device wraps FsHelpers::naturalFileLess so distinct names are never
// "equal". Must yield a STRICT TOTAL order for cursor paging to be correct.
bool ciWeakLess(const std::string& a, const std::string& b) {
  std::string la = a, lb = b;
  std::transform(la.begin(), la.end(), la.begin(), [](unsigned char c) { return std::tolower(c); });
  std::transform(lb.begin(), lb.end(), lb.begin(), [](unsigned char c) { return std::tolower(c); });
  return la < lb;
}
bool ciStrictLess(const std::string& a, const std::string& b) {
  if (ciWeakLess(a, b)) return true;
  if (ciWeakLess(b, a)) return false;
  return a < b;  // tiebreak → strict total order on distinct names
}

std::vector<Entry> makeEntries(const std::vector<std::string>& names) {
  std::vector<Entry> v;
  for (const auto& n : names) v.push_back({n, 0, false});
  return v;
}

// Feed entries (in the given, possibly unsorted, order) to a selector and return its window.
std::vector<std::string> runWindow(WindowSelector::Mode mode, const std::string& cursor, size_t k,
                                   filewindow::NameLess less, const std::vector<Entry>& entries, bool* overflow = nullptr) {
  WindowSelector sel(mode, cursor, k, less);
  for (const auto& e : entries) sel.consider(e);
  if (overflow) *overflow = sel.overflowed();
  std::vector<std::string> out;
  for (const auto& e : sel.window()) out.push_back(e.name);
  return out;
}

// Page forward from First, collecting every window, and assert the concatenation equals
// the fully-sorted distinct name list — no skips, no duplicates, across any feed order.
std::vector<std::string> pageForwardAll(const std::vector<Entry>& entries, size_t k, filewindow::NameLess less) {
  std::vector<std::string> all;
  auto win = runWindow(WindowSelector::Mode::First, "", k, less, entries);
  while (!win.empty()) {
    for (const auto& n : win) all.push_back(n);
    const std::string cursor = win.back();
    win = runWindow(WindowSelector::Mode::After, cursor, k, less, entries);
  }
  return all;
}

}  // namespace

TEST(FileWindowSelectTest, FirstWindowIsSmallestKSorted) {
  const auto e = makeEntries({"banana", "apple", "date", "cherry", "egg"});
  bool of = false;
  const auto w = runWindow(WindowSelector::Mode::First, "", 2, plainLess, e, &of);
  EXPECT_EQ(w, (std::vector<std::string>{"apple", "banana"}));
  EXPECT_TRUE(of);  // more pages follow
}

TEST(FileWindowSelectTest, LastWindowIsLargestKAscending) {
  const auto e = makeEntries({"banana", "apple", "date", "cherry", "egg"});
  bool of = false;
  const auto w = runWindow(WindowSelector::Mode::Last, "", 2, plainLess, e, &of);
  EXPECT_EQ(w, (std::vector<std::string>{"date", "egg"}));
  EXPECT_TRUE(of);  // pages precede it
}

TEST(FileWindowSelectTest, AfterCursorGivesNextPage) {
  const auto e = makeEntries({"banana", "apple", "date", "cherry", "egg"});
  bool of = false;
  const auto w = runWindow(WindowSelector::Mode::After, "banana", 2, plainLess, e, &of);
  EXPECT_EQ(w, (std::vector<std::string>{"cherry", "date"}));
  EXPECT_TRUE(of);  // "egg" still follows
}

TEST(FileWindowSelectTest, AfterCursorLastPageNoOverflow) {
  const auto e = makeEntries({"banana", "apple", "date", "cherry", "egg"});
  bool of = true;
  const auto w = runWindow(WindowSelector::Mode::After, "date", 2, plainLess, e, &of);
  EXPECT_EQ(w, (std::vector<std::string>{"egg"}));
  EXPECT_FALSE(of);  // nothing past "egg"
}

TEST(FileWindowSelectTest, BeforeCursorGivesPrevPageAscending) {
  const auto e = makeEntries({"banana", "apple", "date", "cherry", "egg"});
  bool of = false;
  const auto w = runWindow(WindowSelector::Mode::Before, "date", 2, plainLess, e, &of);
  EXPECT_EQ(w, (std::vector<std::string>{"banana", "cherry"}));
  EXPECT_TRUE(of);  // "apple" precedes
}

TEST(FileWindowSelectTest, AtOrAfterIncludesCursor) {
  const auto e = makeEntries({"banana", "apple", "date", "cherry", "egg"});
  const auto w = runWindow(WindowSelector::Mode::AtOrAfter, "cherry", 2, plainLess, e);
  EXPECT_EQ(w, (std::vector<std::string>{"cherry", "date"}));
}

TEST(FileWindowSelectTest, FolderSmallerThanWindow) {
  const auto e = makeEntries({"b", "a"});
  bool of = true;
  const auto w = runWindow(WindowSelector::Mode::First, "", 10, plainLess, e, &of);
  EXPECT_EQ(w, (std::vector<std::string>{"a", "b"}));
  EXPECT_FALSE(of);
}

TEST(FileWindowSelectTest, EmptyFolder) {
  const auto w = runWindow(WindowSelector::Mode::First, "", 5, plainLess, {});
  EXPECT_TRUE(w.empty());
}

TEST(FileWindowSelectTest, ForwardPagingCoversEverythingNoDupNoSkip) {
  // 50 names fed in shuffled order; page forward with small K must reproduce the full
  // sorted set exactly once.
  std::vector<std::string> names;
  for (int i = 0; i < 50; i++) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "file_%02d", i);
    names.push_back(buf);
  }
  std::vector<std::string> sorted = names;
  std::sort(sorted.begin(), sorted.end());

  std::mt19937 rng(12345);
  std::shuffle(names.begin(), names.end(), rng);
  const auto entries = makeEntries(names);

  for (size_t k : {1u, 3u, 7u, 50u, 100u}) {
    const auto all = pageForwardAll(entries, k, plainLess);
    EXPECT_EQ(all, sorted) << "k=" << k;
  }
}

TEST(FileWindowSelectTest, TiebreakKeepsStrictOrderAcrossPaging) {
  // Names that the weak (case-insensitive) order ties: "Apple"/"apple", "Box"/"box".
  // The strict wrapper must order them deterministically so forward paging still visits
  // every distinct name exactly once.
  const std::vector<std::string> names = {"box", "Apple", "apple", "Box", "cat"};
  std::vector<std::string> sorted = names;
  std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) { return ciStrictLess(a, b); });

  const auto entries = makeEntries(names);
  for (size_t k : {1u, 2u, 3u}) {
    const auto all = pageForwardAll(entries, k, ciStrictLess);
    EXPECT_EQ(all.size(), names.size()) << "k=" << k;            // no skips, no dups
    EXPECT_EQ(all, sorted) << "k=" << k;
  }
}

TEST(FileWindowSelectTest, BackwardPagingFromLastCoversEverything) {
  std::vector<std::string> names;
  for (int i = 0; i < 23; i++) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "n%02d", i);
    names.push_back(buf);
  }
  std::vector<std::string> sorted = names;
  std::sort(sorted.begin(), sorted.end());
  std::mt19937 rng(999);
  std::shuffle(names.begin(), names.end(), rng);
  const auto entries = makeEntries(names);

  const size_t k = 5;
  std::vector<std::string> collected;
  auto win = runWindow(WindowSelector::Mode::Last, "", k, plainLess, entries);
  while (!win.empty()) {
    collected.insert(collected.begin(), win.begin(), win.end());  // prepend each earlier page
    const std::string cursor = win.front();
    win = runWindow(WindowSelector::Mode::Before, cursor, k, plainLess, entries);
  }
  EXPECT_EQ(collected, sorted);
}
