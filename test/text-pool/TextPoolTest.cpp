#include <gtest/gtest.h>

#include <string>

#include "util/TextPool.h"

namespace {

// Read back an entry the way callers do: pool.data() + offset must be a valid C string.
const char* entry(const std::string& pool, uint16_t offset) { return pool.data() + offset; }

}  // namespace

TEST(TextPoolAppendNoThrow, ReturnsOffsetsThatReadBackAsCStrings) {
  std::string pool;
  uint16_t a = 0xFFFF, b = 0xFFFF, c = 0xFFFF;

  ASSERT_TRUE(TextPool::appendNoThrow(pool, "alpha", 5, a));
  ASSERT_TRUE(TextPool::appendNoThrow(pool, "beta", 4, b));
  ASSERT_TRUE(TextPool::appendNoThrow(pool, "", 0, c));

  EXPECT_STREQ(entry(pool, a), "alpha");
  EXPECT_STREQ(entry(pool, b), "beta");
  EXPECT_STREQ(entry(pool, c), "");
}

TEST(TextPoolAppendNoThrow, FirstOffsetIsZeroAndOffsetsAdvancePastTheNull) {
  std::string pool;
  uint16_t a = 0xFFFF, b = 0xFFFF;

  ASSERT_TRUE(TextPool::appendNoThrow(pool, "abc", 3, a));
  ASSERT_TRUE(TextPool::appendNoThrow(pool, "de", 2, b));

  EXPECT_EQ(a, 0u);
  EXPECT_EQ(b, 4u);  // "abc\0" == 4 bytes
  EXPECT_EQ(pool.size(), 7u);
}

TEST(TextPoolAppendNoThrow, MatchesTheThrowingOverloadsLayout) {
  std::string viaThrowing, viaNoThrow;
  uint16_t got = 0xFFFF;

  const uint16_t expected = TextPool::append(viaThrowing, "hello world", 11);
  ASSERT_TRUE(TextPool::appendNoThrow(viaNoThrow, "hello world", 11, got));

  EXPECT_EQ(got, expected);
  EXPECT_EQ(viaNoThrow, viaThrowing);
}

TEST(TextPoolAppendNoThrow, EmbeddedNullsDoNotDisturbLaterOffsets) {
  std::string pool;
  uint16_t a = 0xFFFF, b = 0xFFFF;

  // A pool legitimately contains one null per entry; entries are found by stored
  // offset, never by scanning.
  ASSERT_TRUE(TextPool::appendNoThrow(pool, "x", 1, a));
  ASSERT_TRUE(TextPool::appendNoThrow(pool, "y", 1, b));

  EXPECT_EQ(pool.size(), 4u);
  EXPECT_STREQ(entry(pool, b), "y");
}

// The offset callers store is a uint16_t (PooledSegment::offset). Past 64KB the throwing
// append() truncates it silently and the page renders garbage; appendNoThrow must refuse.
TEST(TextPoolAppendNoThrow, RefusesAppendThatWouldOverflowTheUint16Offset) {
  std::string pool;
  pool.resize(UINT16_MAX - 4, 'x');
  const std::string before = pool;

  uint16_t offset = 0x1234;
  const std::string payload(16, 'y');
  EXPECT_FALSE(TextPool::appendNoThrow(pool, payload.c_str(), payload.size(), offset));

  EXPECT_EQ(pool, before) << "pool must be untouched when the append is refused";
  EXPECT_EQ(offset, 0x1234) << "outOffset must not be written when the append is refused";
}

TEST(TextPoolAppendNoThrow, AcceptsTheLargestAppendThatStillFits) {
  std::string pool;
  pool.resize(UINT16_MAX - 4, 'x');

  // needed == pool.size() + len + 1 must stay <= UINT16_MAX, so len <= 3.
  uint16_t offset = 0;
  EXPECT_TRUE(TextPool::appendNoThrow(pool, "abc", 3, offset));
  EXPECT_EQ(offset, UINT16_MAX - 4);
  EXPECT_STREQ(entry(pool, offset), "abc");
}

// The point of pooling on a fragmenting heap is that N short strings cost far fewer than N
// allocations. The +256 linear step is what buys that, so assert the amortisation directly
// rather than any particular capacity value (which starts at the SSO buffer, not 0).
TEST(TextPoolAppendNoThrow, AmortisesGrowthInsteadOfReallocatingPerAppend) {
  std::string pool;
  uint16_t offset = 0;
  size_t lastCapacity = pool.capacity();
  int growths = 0;

  for (int i = 0; i < 100; i++) {
    ASSERT_TRUE(TextPool::appendNoThrow(pool, "ab", 2, offset));
    if (pool.capacity() != lastCapacity) {
      growths++;
      lastCapacity = pool.capacity();
    }
  }

  // 100 appends x 3 bytes = 300 bytes, so a 256-byte step needs a couple of growths.
  // The value that matters is that it is a small constant, not O(appends).
  EXPECT_LE(growths, 5) << "expected amortised growth, saw " << growths << " reallocations";
  EXPECT_GE(pool.capacity(), 300u);
}

TEST(TextPoolAppendNoThrow, SucceedsAcrossManyGrowthSteps) {
  std::string pool;
  uint16_t first = 0xFFFF, last = 0xFFFF;

  ASSERT_TRUE(TextPool::appendNoThrow(pool, "first", 5, first));
  for (int i = 0; i < 500; i++) {
    ASSERT_TRUE(TextPool::appendNoThrow(pool, "0123456789", 10, last)) << "failed at i=" << i;
  }

  // Earlier offsets stay valid after every reallocation.
  EXPECT_STREQ(entry(pool, first), "first");
  EXPECT_STREQ(entry(pool, last), "0123456789");
}
