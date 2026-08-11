#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "AdvanceTableMerge.h"

namespace {

// Mirrors SdCardFont::AdvanceEntry (uint32 codepoint + 12.4 fixed-point advance).
struct Entry {
  uint32_t codepoint = 0;
  uint16_t advanceX = 0;
};

constexpr uint32_t LIMIT = 768;    // SdCardFont::ADVANCE_CACHE_LIMIT
constexpr uint32_t MIN_CAP = 128;  // SdCardFont::ADVANCE_CACHE_MIN_CAP

// Stands in for SdCardFont's advanceTable_/Size_/Cap_ triple plus mergeIntoAdvanceTable's
// control flow, so the test exercises the same decisions the firmware makes. Counts
// reallocations — the whole point of the change.
class Table {
 public:
  // Returns false when a merge was rejected outright (table already at the cap).
  bool merge(const std::vector<Entry>& sortedNew) {
    if (sortedNew.empty()) return true;
    if (size_ >= LIMIT) return false;

    const uint32_t needed = AdvanceTableMerge::mergedSize(size_, sortedNew.size(), LIMIT);

    if (needed <= cap_) {
      AdvanceTableMerge::mergeInPlace(buf_.data(), size_, sortedNew.data(), static_cast<uint32_t>(sortedNew.size()),
                                      needed);
      size_ = needed;
      return true;
    }

    const uint32_t newCap = AdvanceTableMerge::nextCapacity(cap_, needed, MIN_CAP, LIMIT);
    std::vector<Entry> grown(newCap);
    const uint32_t written = AdvanceTableMerge::mergeForward(grown.data(), needed, buf_.data(), size_, sortedNew.data(),
                                                             static_cast<uint32_t>(sortedNew.size()));
    buf_ = std::move(grown);
    size_ = written;
    cap_ = newCap;
    reallocs_++;
    return true;
  }

  uint32_t size() const { return size_; }
  uint32_t cap() const { return cap_; }
  int reallocs() const { return reallocs_; }

  std::vector<Entry> contents() const { return {buf_.begin(), buf_.begin() + size_}; }

  bool isSortedUnique() const {
    for (uint32_t i = 1; i < size_; i++) {
      if (buf_[i - 1].codepoint >= buf_[i].codepoint) return false;
    }
    return true;
  }

  // Every entry must still carry the advance it was merged with — catches a merge that
  // copies the wrong element.
  bool advancesMatchCodepoints() const {
    for (uint32_t i = 0; i < size_; i++) {
      if (buf_[i].advanceX != static_cast<uint16_t>(buf_[i].codepoint * 2)) return false;
    }
    return true;
  }

 private:
  std::vector<Entry> buf_;
  uint32_t size_ = 0;
  uint32_t cap_ = 0;
  int reallocs_ = 0;
};

Entry make(const uint32_t cp) { return Entry{cp, static_cast<uint16_t>(cp * 2)}; }

}  // namespace

TEST(AdvanceTableMerge, MergedSizeTruncatesAtLimit) {
  EXPECT_EQ(AdvanceTableMerge::mergedSize(0, 10, LIMIT), 10u);
  EXPECT_EQ(AdvanceTableMerge::mergedSize(700, 50, LIMIT), 750u);
  EXPECT_EQ(AdvanceTableMerge::mergedSize(700, 100, LIMIT), LIMIT);
}

TEST(AdvanceTableMerge, NextCapacityDoublesFromMinAndClampsToLimit) {
  EXPECT_EQ(AdvanceTableMerge::nextCapacity(0, 1, MIN_CAP, LIMIT), 128u);
  EXPECT_EQ(AdvanceTableMerge::nextCapacity(128, 129, MIN_CAP, LIMIT), 256u);
  EXPECT_EQ(AdvanceTableMerge::nextCapacity(256, 257, MIN_CAP, LIMIT), 512u);
  EXPECT_EQ(AdvanceTableMerge::nextCapacity(512, 513, MIN_CAP, LIMIT), LIMIT);
  // A first merge larger than the growth floor jumps straight to what it needs.
  EXPECT_EQ(AdvanceTableMerge::nextCapacity(0, 300, MIN_CAP, LIMIT), 300u);
  // Never exceeds the limit even when doubling would.
  EXPECT_EQ(AdvanceTableMerge::nextCapacity(512, 768, MIN_CAP, LIMIT), LIMIT);
}

// The regression this whole change exists for: the device merges ONE codepoint at a time
// ("Advance table style 0: +1 from SD, total=65/768"). That must not realloc per merge.
TEST(AdvanceTableMerge, IncrementalSingleCodepointMergesStayCheap) {
  Table t;
  for (uint32_t cp = 1; cp <= LIMIT; cp++) {
    ASSERT_TRUE(t.merge({make(cp)})) << "rejected at cp=" << cp;
  }

  EXPECT_EQ(t.size(), LIMIT);
  EXPECT_TRUE(t.isSortedUnique());
  EXPECT_TRUE(t.advancesMatchCodepoints());
  // 128 -> 256 -> 512 -> 768. The old code reallocated on all 768 merges.
  EXPECT_EQ(t.reallocs(), 4);
  EXPECT_EQ(t.cap(), LIMIT);
}

// Interleaved codepoints exercise the backward in-place merge's overlap handling: new entries
// land between resident ones, so entries must shift up without clobbering unread elements.
TEST(AdvanceTableMerge, InterleavedMergePreservesOrderAndPayload) {
  Table t;
  for (uint32_t cp = 0; cp < 200; cp += 2) ASSERT_TRUE(t.merge({make(cp)}));  // evens
  const int afterEvens = t.reallocs();

  for (uint32_t cp = 1; cp < 200; cp += 2) ASSERT_TRUE(t.merge({make(cp)}));  // odds, interleaving

  EXPECT_EQ(t.size(), 200u);
  EXPECT_TRUE(t.isSortedUnique());
  EXPECT_TRUE(t.advancesMatchCodepoints());

  const auto contents = t.contents();
  for (uint32_t i = 0; i < 200; i++) EXPECT_EQ(contents[i].codepoint, i);
  EXPECT_LE(t.reallocs() - afterEvens, 1);
}

TEST(AdvanceTableMerge, BatchMergeOfManyCodepointsAtOnce) {
  Table t;
  std::vector<Entry> batch;
  for (uint32_t cp = 100; cp < 400; cp++) batch.push_back(make(cp));
  ASSERT_TRUE(t.merge(batch));

  EXPECT_EQ(t.size(), 300u);
  EXPECT_EQ(t.cap(), 300u);  // first merge sizes exactly to need when it exceeds the floor
  EXPECT_EQ(t.reallocs(), 1);
  EXPECT_TRUE(t.isSortedUnique());

  std::vector<Entry> lower;
  for (uint32_t cp = 0; cp < 50; cp++) lower.push_back(make(cp));
  ASSERT_TRUE(t.merge(lower));  // all below the resident range — worst case for shifting

  EXPECT_EQ(t.size(), 350u);
  EXPECT_TRUE(t.isSortedUnique());
  EXPECT_TRUE(t.advancesMatchCodepoints());
  EXPECT_EQ(t.contents().front().codepoint, 0u);
  EXPECT_EQ(t.contents().back().codepoint, 399u);
}

// Truncation must drop the LARGEST codepoints, on both the in-place and the grow path.
TEST(AdvanceTableMerge, TruncationDropsTailOnGrowPath) {
  Table t;
  std::vector<Entry> first;
  for (uint32_t cp = 0; cp < LIMIT - 10; cp++) first.push_back(make(cp));
  ASSERT_TRUE(t.merge(first));
  ASSERT_EQ(t.size(), LIMIT - 10);

  std::vector<Entry> more;
  for (uint32_t cp = LIMIT - 10; cp < LIMIT + 100; cp++) more.push_back(make(cp));
  ASSERT_TRUE(t.merge(more));

  EXPECT_EQ(t.size(), LIMIT);
  EXPECT_TRUE(t.isSortedUnique());
  EXPECT_EQ(t.contents().back().codepoint, LIMIT - 1);
}

TEST(AdvanceTableMerge, TruncationDropsTailOnInPlacePath) {
  std::vector<Entry> buf(LIMIT);
  for (uint32_t i = 0; i < 100; i++) buf[i] = make(i * 10);  // 0,10,...,990

  std::vector<Entry> incoming;
  for (uint32_t cp = 0; cp < LIMIT; cp++) incoming.push_back(make(cp * 10 + 5));  // interleaves

  const uint32_t needed = AdvanceTableMerge::mergedSize(100, incoming.size(), LIMIT);
  ASSERT_EQ(needed, LIMIT);
  AdvanceTableMerge::mergeInPlace(buf.data(), 100, incoming.data(), static_cast<uint32_t>(incoming.size()), needed);

  for (uint32_t i = 1; i < LIMIT; i++) EXPECT_LT(buf[i - 1].codepoint, buf[i].codepoint) << "at " << i;
  EXPECT_EQ(buf[0].codepoint, 0u);
  for (uint32_t i = 0; i < LIMIT; i++) EXPECT_EQ(buf[i].advanceX, static_cast<uint16_t>(buf[i].codepoint * 2));
}

TEST(AdvanceTableMerge, MergeIntoEmptyTable) {
  std::vector<Entry> buf(4);
  const std::vector<Entry> incoming = {make(7), make(9)};
  const uint32_t needed = AdvanceTableMerge::mergedSize(0, incoming.size(), LIMIT);
  AdvanceTableMerge::mergeInPlace(buf.data(), 0, incoming.data(), 2, needed);

  EXPECT_EQ(needed, 2u);
  EXPECT_EQ(buf[0].codepoint, 7u);
  EXPECT_EQ(buf[1].codepoint, 9u);
}

TEST(AdvanceTableMerge, MergeForwardMatchesInPlaceResult) {
  std::vector<Entry> resident;
  for (uint32_t cp = 0; cp < 60; cp += 3) resident.push_back(make(cp));
  std::vector<Entry> incoming;
  for (uint32_t cp = 1; cp < 60; cp += 3) incoming.push_back(make(cp));

  const uint32_t needed = AdvanceTableMerge::mergedSize(static_cast<uint32_t>(resident.size()),
                                                        static_cast<uint32_t>(incoming.size()), LIMIT);

  std::vector<Entry> forwardDst(needed);
  const uint32_t written = AdvanceTableMerge::mergeForward(forwardDst.data(), needed, resident.data(),
                                                           static_cast<uint32_t>(resident.size()), incoming.data(),
                                                           static_cast<uint32_t>(incoming.size()));
  ASSERT_EQ(written, needed);

  std::vector<Entry> inPlaceBuf = resident;
  inPlaceBuf.resize(needed);
  AdvanceTableMerge::mergeInPlace(inPlaceBuf.data(), static_cast<uint32_t>(resident.size()), incoming.data(),
                                  static_cast<uint32_t>(incoming.size()), needed);

  for (uint32_t i = 0; i < needed; i++) {
    EXPECT_EQ(forwardDst[i].codepoint, inPlaceBuf[i].codepoint) << "at " << i;
    EXPECT_EQ(forwardDst[i].advanceX, inPlaceBuf[i].advanceX) << "at " << i;
  }
}
