#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "OpdsStringArena.h"

namespace {

TEST(OpdsStringArena, StoresNullTerminatedText) {
  OpdsStringArena arena;
  const char* s = arena.add(std::string("Moby-Dick"));
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "Moby-Dick");
}

TEST(OpdsStringArena, EmptyStringCostsNoArenaSpace) {
  OpdsStringArena arena;
  const char* s = arena.add(std::string());
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "");
  EXPECT_EQ(arena.chunkCount(), 0u);
  EXPECT_EQ(arena.bytesUsed(), 0u);
}

// The property the whole design rests on: entries hold bare pointers, so text must not
// move as more of it is added.
TEST(OpdsStringArena, PointersStayValidAsTheArenaGrows) {
  OpdsStringArena arena;
  std::vector<const char*> handles;
  std::vector<std::string> expected;
  for (int i = 0; i < 200; i++) {
    expected.push_back("entry-title-number-" + std::to_string(i));
    const char* p = arena.add(expected.back());
    ASSERT_NE(p, nullptr) << "failed at i=" << i;
    handles.push_back(p);
  }
  ASSERT_GT(arena.chunkCount(), 1u) << "test is meaningless if it all fit in one chunk";
  for (size_t i = 0; i < handles.size(); i++) EXPECT_STREQ(handles[i], expected[i].c_str());
}

// A string must never straddle a chunk, or the pointer handed back would not be a C string.
TEST(OpdsStringArena, StringSpanningAChunkBoundaryStartsAFreshChunk) {
  OpdsStringArena arena;
  const std::string filler(OpdsStringArena::CHUNK_BYTES - 40, 'f');
  const std::string tail(60, 't');
  const char* a = arena.add(filler);
  const char* b = arena.add(tail);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_STREQ(a, filler.c_str());
  EXPECT_STREQ(b, tail.c_str());
  EXPECT_EQ(arena.chunkCount(), 2u);
}

TEST(OpdsStringArena, StringLongerThanAChunkGetsItsOwnChunk) {
  OpdsStringArena arena;
  const std::string huge(OpdsStringArena::CHUNK_BYTES * 2 + 17, 'h');
  const char* p = arena.add(huge);
  ASSERT_NE(p, nullptr);
  EXPECT_STREQ(p, huge.c_str());
  EXPECT_EQ(strlen(p), huge.size());
}

TEST(OpdsStringArena, ClearReleasesEverything) {
  OpdsStringArena arena;
  for (int i = 0; i < 50; i++) arena.add(std::string(100, 'x'));
  ASSERT_GT(arena.chunkCount(), 0u);
  arena.clear();
  EXPECT_EQ(arena.chunkCount(), 0u);
  EXPECT_EQ(arena.bytesUsed(), 0u);
  // Reusable after a clear — a feed reload takes this path on every navigation.
  const char* s = arena.add(std::string("after-clear"));
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "after-clear");
}

// A whole feed's worth of text has to fit, since removing the truncation is the point.
TEST(OpdsStringArena, HoldsAFullSixtyFourEntryFeed) {
  OpdsStringArena arena;
  for (int i = 0; i < 64; i++) {
    ASSERT_NE(arena.add(std::string(58, 'T')), nullptr) << "title " << i;
    ASSERT_NE(arena.add(std::string(24, 'A')), nullptr) << "author " << i;
    ASSERT_NE(arena.add(std::string(72, 'H')), nullptr) << "href " << i;
  }
  EXPECT_GE(arena.bytesUsed(), 64u * (58 + 24 + 72));
}

TEST(OpdsStringArena, MoveTransfersOwnershipAndKeepsPointersValid) {
  OpdsStringArena source;
  const char* p = source.add(std::string("survives-the-move"));
  ASSERT_NE(p, nullptr);
  const size_t bytes = source.bytesUsed();

  OpdsStringArena moved = std::move(source);
  EXPECT_STREQ(p, "survives-the-move");
  EXPECT_EQ(moved.bytesUsed(), bytes);
}

}  // namespace
