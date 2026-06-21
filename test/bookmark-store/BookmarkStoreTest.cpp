#include <esp_rom_crc.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "BookmarkStore.h"
#include "HalStorage.h"

namespace {

// Mirror of the store's path derivation so tests can hand-write legacy files and inspect
// sidecars directly. Kept in sync with BookmarkStore::loadForBook.
std::string crcName(const std::string& bookPath, const char* bookType, const char* ext) {
  const uint32_t crc =
      esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(bookPath.data()), static_cast<uint32_t>(bookPath.size()));
  return std::string("/.crosspoint/bookmarks/") + bookType + "_" + std::to_string(crc) + ext;
}

class BookmarkStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = (std::filesystem::temp_directory_path() /
             ("bkstore_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
                .string();
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    HalStorage::getInstance().setRoot(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::string realPath(const std::string& devicePath) const { return root_ + devicePath; }

  // Write a v7-format bookmark file (pre-quote) by hand to exercise migration.
  struct V7Point {
    uint16_t spine;
    float progress;
    uint32_t version;
    std::string chapter;
  };
  void writeV7File(const std::string& bookPath, const std::vector<V7Point>& pts) {
    const std::string p = realPath(crcName(bookPath, "epub", ".bin"));
    std::filesystem::create_directories(std::filesystem::path(p).parent_path());
    std::ofstream f(p, std::ios::binary);
    auto pod = [&](auto v) { f.write(reinterpret_cast<const char*>(&v), sizeof(v)); };
    auto str = [&](const std::string& s) {
      uint32_t len = static_cast<uint32_t>(s.size());
      pod(len);
      f.write(s.data(), len);
    };
    pod(static_cast<uint8_t>(7));            // VERSION 7
    pod(static_cast<uint16_t>(pts.size()));  // count
    str("Title");
    str("Author");
    str(bookPath);  // embedded path (must match for load)
    for (const auto& pt : pts) {
      pod(pt.spine);
      pod(pt.progress);
      pod(pt.version);
      char chapter[48] = {};
      snprintf(chapter, sizeof(chapter), "%s", pt.chapter.c_str());
      f.write(chapter, sizeof(chapter));
      pod(static_cast<uint16_t>(UINT16_MAX));  // paragraphIndex
      char snippet[64] = {};
      f.write(snippet, sizeof(snippet));
      pod(static_cast<uint8_t>(0));   // returnFlag
      pod(static_cast<uint16_t>(0));  // chapterCurrentPage
      pod(static_cast<uint16_t>(0));  // chapterPageCount
    }
  }

  std::string root_;
};

TEST_F(BookmarkStoreTest, QuoteRoundTripPersistsRangeAndPreview) {
  const std::string book = "/books/pride.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "Pride", "Austen", "epub"));

  const std::string longText(300, 'x');  // > snippet, < QUOTE_PREVIEW_MAX
  ASSERT_EQ(store.addQuote(3, 0.42f, 5, 9, 40, "Chapter 3", longText.c_str(), 17), BookmarkStore::AddResult::Added);
  store.unload();

  ASSERT_TRUE(store.loadForBook(book, "Pride", "Austen", "epub"));
  ASSERT_EQ(store.getBookmarks().size(), 1u);
  const Bookmark& bm = store.getBookmarks()[0];
  EXPECT_TRUE(bm.isQuote());
  EXPECT_EQ(bm.spineIndex, 3);
  EXPECT_EQ(bm.startWord, 5);
  EXPECT_EQ(bm.endWord, 9);
  EXPECT_FLOAT_EQ(bm.progress, 0.42f);

  std::string preview;
  EXPECT_TRUE(store.readPreviewAt(0, preview));
  EXPECT_EQ(preview, longText);  // full text recovered from .qtext
}

TEST_F(BookmarkStoreTest, PointBookmarkIsNotAQuote) {
  const std::string book = "/books/a.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "A", "B", "epub"));
  ASSERT_EQ(store.addBookmark(2, 0.5f, 10, "Ch2"), BookmarkStore::AddResult::Added);
  store.unload();

  ASSERT_TRUE(store.loadForBook(book, "A", "B", "epub"));
  ASSERT_EQ(store.getBookmarks().size(), 1u);
  EXPECT_FALSE(store.getBookmarks()[0].isQuote());
  std::string preview;
  EXPECT_FALSE(store.readPreviewAt(0, preview));  // no preview for a point bookmark
}

TEST_F(BookmarkStoreTest, TwoQuotesSamePageDoNotCollapseOnMerge) {
  const std::string book = "/books/c.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "C", "D", "epub"));
  // Same spine + same progress, different word ranges → distinct quote identities.
  ASSERT_EQ(store.addQuote(4, 0.30f, 1, 3, 20, "Ch4", "first", 5), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(4, 0.30f, 8, 12, 20, "Ch4", "second", 5), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.getBookmarks().size(), 2u);

  // Round-trip through the sync blob and merge back into a fresh store.
  const std::string blob = BookmarkStore::serializeToJson(store.getBookmarks(), store.getTombstones());
  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  ASSERT_TRUE(BookmarkStore::parseFromJson(blob.c_str(), bms, tombs));
  EXPECT_EQ(bms.size(), 2u);

  const std::string book2 = "/books/c2.epub";
  ASSERT_TRUE(store.loadForBook(book2, "C", "D", "epub"));  // empty store
  store.mergeFrom(bms, tombs);
  EXPECT_EQ(store.getBookmarks().size(), 2u) << "two same-page quotes must remain distinct";
}

TEST_F(BookmarkStoreTest, JsonCarriesRangeAndSnippetNotFullPreview) {
  const std::string book = "/books/e.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "E", "F", "epub"));
  const std::string longText(400, 'z');
  ASSERT_EQ(store.addQuote(1, 0.1f, 2, 7, 15, "Ch1", longText.c_str(), 0), BookmarkStore::AddResult::Added);

  const std::string blob = BookmarkStore::serializeToJson(store.getBookmarks(), store.getTombstones());
  // Short-key wire format (see the key map in BookmarkStore::serializeToJson).
  EXPECT_NE(blob.find("\"q\":true"), std::string::npos);
  EXPECT_NE(blob.find("\"sw\":2"), std::string::npos);
  EXPECT_NE(blob.find("\"ew\":7"), std::string::npos);
  // The full 400-char preview must NOT cross the wire (snippet teaser only).
  EXPECT_EQ(blob.find(longText), std::string::npos);
  EXPECT_LT(blob.size(), longText.size());
}

TEST_F(BookmarkStoreTest, ParsesLegacyLongKeyBlob) {
  // A blob from older firmware (verbose long keys) must still parse — during a mixed-version
  // window a peer may upload long keys before it is updated. parseFromJson reads both.
  const char* legacy =
      "{\"bookmarks\":[{\"spineIndex\":5,\"progress\":0.5,\"version\":3,\"chapterTitle\":\"Ch5\","
      "\"paragraphIndex\":12,\"snippet\":\"hello\",\"chapterCurrentPage\":2,\"chapterPageCount\":10,"
      "\"quote\":true,\"endSpineIndex\":5,\"endProgress\":0.6,\"startWord\":4,\"endWord\":9}],"
      "\"tombstones\":[{\"spineIndex\":7,\"paragraphIndex\":3,\"progress\":0.7,\"version\":2}]}";
  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  ASSERT_TRUE(BookmarkStore::parseFromJson(legacy, bms, tombs));
  ASSERT_EQ(bms.size(), 1u);
  EXPECT_EQ(bms[0].spineIndex, 5);
  EXPECT_FLOAT_EQ(bms[0].progress, 0.5f);
  EXPECT_EQ(bms[0].version, 3u);
  EXPECT_STREQ(bms[0].chapterTitle, "Ch5");
  EXPECT_EQ(bms[0].paragraphIndex, 12);
  EXPECT_STREQ(bms[0].snippet, "hello");
  EXPECT_EQ(bms[0].chapterCurrentPage, 2);
  EXPECT_EQ(bms[0].chapterPageCount, 10);
  EXPECT_TRUE(bms[0].isQuote());
  EXPECT_EQ(bms[0].startWord, 4);
  EXPECT_EQ(bms[0].endWord, 9);
  ASSERT_EQ(tombs.size(), 1u);
  EXPECT_EQ(tombs[0].spineIndex, 7);
  EXPECT_EQ(tombs[0].paragraphIndex, 3);
  EXPECT_EQ(tombs[0].version, 2u);
}

TEST_F(BookmarkStoreTest, LegacyTimestampKeyMapsToVersion) {
  // Oldest blobs used "timestamp" for what is now "version"; the triple fallback keeps it.
  const char* oldest = "[{\"spineIndex\":1,\"progress\":0.2,\"timestamp\":42,\"chapterTitle\":\"X\"}]";
  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  ASSERT_TRUE(BookmarkStore::parseFromJson(oldest, bms, tombs));
  ASSERT_EQ(bms.size(), 1u);
  EXPECT_EQ(bms[0].version, 42u);
}

TEST_F(BookmarkStoreTest, RemoveQuoteCompactsPreview) {
  const std::string book = "/books/g.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "G", "H", "epub"));
  ASSERT_EQ(store.addQuote(1, 0.1f, 1, 2, 10, "Ch1", "alpha-text", 0), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(1, 0.2f, 5, 6, 10, "Ch1", "beta-text", 0), BookmarkStore::AddResult::Added);

  EXPECT_TRUE(store.removeQuoteByRange(1, 1, 2));
  ASSERT_EQ(store.getBookmarks().size(), 1u);

  // Surviving quote's preview still readable; removed one's key returns nothing.
  std::string preview;
  ASSERT_TRUE(store.readPreviewAt(0, preview));
  EXPECT_EQ(preview, "beta-text");
}

TEST_F(BookmarkStoreTest, MigratesV7FileToV8AsPointBookmarks) {
  const std::string book = "/books/legacy.epub";
  writeV7File(book, {{1, 0.10f, 0, "Chap One"}, {2, 0.50f, 0, "Chap Two"}});

  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "Legacy", "Auth", "epub"));
  ASSERT_EQ(store.getBookmarks().size(), 2u);
  for (const auto& bm : store.getBookmarks()) {
    EXPECT_FALSE(bm.isQuote()) << "migrated v7 records are point bookmarks";
  }
  store.unload();  // re-save migrates to v8 on disk

  // Reload: still valid, still point bookmarks (now from a v8 file).
  ASSERT_TRUE(store.loadForBook(book, "Legacy", "Auth", "epub"));
  EXPECT_EQ(store.getBookmarks().size(), 2u);
  const std::string p = realPath(crcName(book, "epub", ".bin"));
  std::ifstream f(p, std::ios::binary);
  uint8_t ver = 0;
  f.read(reinterpret_cast<char*>(&ver), 1);
  EXPECT_EQ(ver, 8) << "file migrated to v8 on save";
}

TEST_F(BookmarkStoreTest, ExportTxtContainsQuotesOnly) {
  const std::string book = "/books/export me.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "ExportTitle", "ExportAuthor", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.2f, 10, "Ch1"), BookmarkStore::AddResult::Added);  // page bookmark
  ASSERT_EQ(store.addQuote(2, 0.5f, 3, 8, 12, "Ch2", "the-quoted-text", 4), BookmarkStore::AddResult::Added);

  const std::string txtPath = realPath("/highlights/export me.txt");
  ASSERT_TRUE(std::filesystem::exists(txtPath));
  std::ifstream f(txtPath);
  const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  EXPECT_NE(body.find("the-quoted-text"), std::string::npos);
  EXPECT_NE(body.find("ExportTitle"), std::string::npos);
  EXPECT_EQ(body.find("page bookmark"), std::string::npos) << "page bookmarks are excluded from highlights export";
  EXPECT_NE(body.find("1 highlight"), std::string::npos);
}

TEST_F(BookmarkStoreTest, PageBookmarkToggleDoesNotRemoveQuote) {
  const std::string book = "/books/toggle.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));

  // A quote and a point bookmark on the same page (progress 0.50, 10-page chapter).
  ASSERT_EQ(store.addQuote(1, 0.50f, 2, 6, 10, "Ch1", "quoted-text", 5), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(1, 0.50f, 10, "Ch1"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.getBookmarks().size(), 2u);

  // The page-bookmark toggle (hold-left) must remove only the point bookmark, leaving the quote.
  store.removeBookmarkForPage(1, 0.50f, 10);
  ASSERT_EQ(store.getBookmarks().size(), 1u);
  EXPECT_TRUE(store.getBookmarks()[0].isQuote());

  EXPECT_TRUE(store.hasQuoteForPage(1, 0.50f, 10));
  EXPECT_FALSE(store.hasPointBookmarkForPage(1, 0.50f, 10));
}

TEST_F(BookmarkStoreTest, EnforcesCombinedCap) {
  const std::string book = "/books/cap.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "Cap", "Auth", "epub"));
  // Fill with quotes up to the cap, then expect refusal.
  int added = 0;
  for (int i = 0; i < 200; i++) {
    const auto r =
        store.addQuote(1, 0.01f * i, static_cast<uint16_t>(i * 2), static_cast<uint16_t>(i * 2 + 1), 250, "Ch", "t", 0);
    if (r == BookmarkStore::AddResult::Added) {
      added++;
    } else {
      EXPECT_EQ(r, BookmarkStore::AddResult::LimitReached);
      break;
    }
  }
  EXPECT_LE(added, 128) << "combined cap must bound the resident set";
  EXPECT_EQ(added, 128);
}

}  // namespace
