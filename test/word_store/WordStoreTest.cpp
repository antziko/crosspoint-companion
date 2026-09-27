#include <Epub/WordStore.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

WordStore::StoredWord put(WordStore& store, const std::string& text) {
  WordStore::StoredWord w;
  EXPECT_TRUE(store.append(text.data(), text.size(), w));
  return w;
}

}  // namespace

TEST(WordStore, StoredWordsAreNulTerminatedAndReadBack) {
  WordStore store;
  const auto a = put(store, "alpha");
  const auto b = put(store, "beta");
  EXPECT_EQ(store.view(a), "alpha");
  EXPECT_EQ(store.view(b), "beta");
  // The C-string form is what feeds drawText/startsWithRtl, so the terminator matters.
  EXPECT_STREQ(store.cstr(a), "alpha");
  EXPECT_STREQ(store.cstr(b), "beta");
}

// An empty token still has to round-trip: addWord() can produce one, and a null data
// pointer would reach a C API.
TEST(WordStore, EmptyWordRoundTrips) {
  WordStore store;
  const auto empty = put(store, "");
  EXPECT_EQ(store.view(empty).size(), 0u);
  EXPECT_STREQ(store.cstr(empty), "");
}

// A paragraph outgrows one 2 KB chunk; every earlier word must stay readable, because
// layout measures words long after they were appended.
TEST(WordStore, WordsSurviveChunkGrowth) {
  WordStore store;
  std::vector<WordStore::StoredWord> handles;
  std::vector<std::string> expected;
  for (int i = 0; i < 400; ++i) {
    expected.push_back("word" + std::to_string(i));
    handles.push_back(put(store, expected.back()));
  }
  EXPECT_GT(store.chunkCount(), 1u);
  for (size_t i = 0; i < handles.size(); ++i) {
    EXPECT_EQ(store.view(handles[i]), expected[i]) << "at index " << i;
  }
}

// Releasing every word in a filled chunk retires it, which is what keeps a chapter-long
// paragraph from holding the whole chapter's text at once.
TEST(WordStore, FullyReleasedChunksAreRetired) {
  WordStore store;
  std::vector<WordStore::StoredWord> handles;
  for (int i = 0; i < 400; ++i) handles.push_back(put(store, "word" + std::to_string(i)));
  const size_t chunks = store.chunkCount();
  ASSERT_GT(chunks, 1u);
  ASSERT_NE(store.chunkData(0), nullptr);

  for (const auto& h : handles) store.release(h);

  // The tail chunk is still accepting appends and is deliberately kept; every earlier one
  // should have given its memory back.
  size_t retired = 0;
  for (size_t i = 0; i + 1 < chunks; ++i) {
    if (store.chunkData(i) == nullptr) ++retired;
  }
  EXPECT_EQ(retired, chunks - 1);
  EXPECT_NE(store.chunkData(chunks - 1), nullptr);
}

// suffix() aliases the original's bytes and inherits its release obligation: the
// hyphenation split relies on releasing exactly one of the pair, never both.
TEST(WordStore, SuffixAliasesTheOriginalBytes) {
  WordStore store;
  const auto whole = put(store, "hyphenation");
  const auto tail = WordStore::suffix(whole, 6);
  EXPECT_EQ(store.view(tail), "ation");
  EXPECT_STREQ(store.cstr(tail), "ation");  // shares the original's trailing NUL
  EXPECT_EQ(store.view(whole), "hyphenation");
  // Releasing the suffix alone must retire the entry exactly once. Releasing the original
  // too would double-decrement and retire a chunk still holding live words.
  store.release(tail);
}

// A word larger than one chunk gets its own exact-fit chunk; the offset arithmetic has to
// stay correct across that special case.
TEST(WordStore, OversizedWordGetsItsOwnChunk) {
  WordStore store;
  const std::string small = "before";
  const auto first = put(store, small);
  const std::string huge(4000, 'x');
  const auto big = put(store, huge);
  EXPECT_EQ(store.view(first), small);
  EXPECT_EQ(store.view(big).size(), huge.size());
  EXPECT_EQ(store.view(big), huge);
}
