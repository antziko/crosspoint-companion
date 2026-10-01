#include <ArduinoJson.h>
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

TEST_F(BookmarkStoreTest, MigratesAV7FileToTheCurrentFormatAsPointBookmarks) {
  const std::string book = "/books/legacy.epub";
  writeV7File(book, {{1, 0.10f, 0, "Chap One"}, {2, 0.50f, 0, "Chap Two"}});

  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "Legacy", "Auth", "epub"));
  ASSERT_EQ(store.getBookmarks().size(), 2u);
  for (const auto& bm : store.getBookmarks()) {
    EXPECT_FALSE(bm.isQuote()) << "migrated v7 records are point bookmarks";
  }
  store.unload();  // re-save migrates to the current format on disk

  // Reload: still valid, still point bookmarks (now from a migrated file).
  ASSERT_TRUE(store.loadForBook(book, "Legacy", "Auth", "epub"));
  EXPECT_EQ(store.getBookmarks().size(), 2u);
  const std::string p = realPath(crcName(book, "epub", ".bin"));
  std::ifstream f(p, std::ios::binary);
  uint8_t ver = 0;
  f.read(reinterpret_cast<char*>(&ver), 1);
  // Not a literal: the point is that a legacy file is rewritten in whatever the current
  // format is, and pinning the number here only breaks on the next field added.
  EXPECT_GE(ver, 8) << "file migrated on save";
  EXPECT_TRUE(store.getBookmarks()[0].visibleTextOffset == 0u) << "no offset in a legacy record";
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

TEST_F(BookmarkStoreTest, ReturnMarkForPageIsConsumedOnlyOnItsOwnPage) {
  const std::string book = "/books/return.epub";
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook(book, "R", "A", "epub"));

  // A return mark on page 5 of a 10-page chapter, plus a normal bookmark on page 2.
  ASSERT_EQ(store.addBookmark(1, 0.50f, 10, "Ch1", UINT16_MAX, nullptr, /*returnMark=*/true, 5),
            BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(1, 0.20f, 10, "Ch1", UINT16_MAX, nullptr, /*returnMark=*/false, 2),
            BookmarkStore::AddResult::Added);
  ASSERT_TRUE(store.isReturnMarkForPage(1, 0.50f, 10));

  // Rendering any other page leaves it alone — including the page holding a normal bookmark,
  // which must never be consumed by arriving at it.
  EXPECT_FALSE(store.removeReturnMarkForPage(1, 0.40f, 10));
  EXPECT_FALSE(store.removeReturnMarkForPage(1, 0.20f, 10));
  EXPECT_FALSE(store.removeReturnMarkForPage(2, 0.50f, 10));
  EXPECT_EQ(store.getBookmarks().size(), 2u);

  // Arriving at its own page consumes it, once.
  EXPECT_TRUE(store.removeReturnMarkForPage(1, 0.50f, 10));
  EXPECT_FALSE(store.isReturnMarkForPage(1, 0.50f, 10));
  EXPECT_FALSE(store.removeReturnMarkForPage(1, 0.50f, 10));

  // The normal bookmark survives.
  ASSERT_EQ(store.getBookmarks().size(), 1u);
  EXPECT_FALSE(store.getBookmarks()[0].returnMark);
  EXPECT_TRUE(store.hasPointBookmarkForPage(1, 0.20f, 10));
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

// A blob holding more records than this device can represent must be flagged, so sync
// suppresses the PUT rather than replacing the server set with its truncated merge.
TEST_F(BookmarkStoreTest, ReportsTruncationWhenRemoteExceedsCap) {
  std::string blob = "{\"b\":[";
  for (int i = 0; i < MAX_BOOKMARKS + 5; i++) {
    if (i) blob += ',';
    blob += "{\"s\":0,\"p\":" + std::to_string(i / 1000.0f) + ",\"v\":1}";
  }
  blob += "],\"t\":[]}";

  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  bool truncated = false;
  ASSERT_TRUE(BookmarkStore::parseFromJson(blob.c_str(), bms, tombs, &truncated));

  EXPECT_EQ(bms.size(), static_cast<size_t>(MAX_BOOKMARKS)) << "parse still caps the vector";
  EXPECT_TRUE(truncated) << "the dropped records must be reported to the caller";
}

TEST_F(BookmarkStoreTest, ReportsTruncationWhenRemoteTombstonesExceedCap) {
  std::string blob = "{\"b\":[],\"t\":[";
  for (int i = 0; i < MAX_BOOKMARKS + 1; i++) {
    if (i) blob += ',';
    blob += "{\"s\":0,\"pi\":" + std::to_string(i) + ",\"p\":0.1,\"v\":2}";
  }
  blob += "]}";

  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  bool truncated = false;
  ASSERT_TRUE(BookmarkStore::parseFromJson(blob.c_str(), bms, tombs, &truncated));

  EXPECT_TRUE(truncated) << "a dropped tombstone would resurrect a deleted bookmark on upload";
}

TEST_F(BookmarkStoreTest, DoesNotReportTruncationForASetThatFits) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/fits.epub";
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.25f, 10, "Ch1"), BookmarkStore::AddResult::Added);

  const std::string blob = BookmarkStore::serializeToJson(store.getBookmarks(), store.getTombstones());
  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  bool truncated = true;  // must be cleared on entry
  ASSERT_TRUE(BookmarkStore::parseFromJson(blob.c_str(), bms, tombs, &truncated));

  EXPECT_FALSE(truncated);
}

// --- KOReader XPath sidecar -------------------------------------------------

TEST_F(BookmarkStoreTest, StoresAndReadsBackAnXPathPerBookmark) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp1.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(3, 0.25f, 10, "Ch3", 42), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(5, 0.50f, 2, 7, 10, "Ch5", "quoted text"), BookmarkStore::AddResult::Added);
  const auto& bms = store.getBookmarks();
  ASSERT_EQ(bms.size(), 2u);

  ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[4]/body/p[42]"));
  ASSERT_TRUE(store.setXPath(bms[1], "/body/DocFragment[6]/body/p[9]/text()[1].12"));

  std::string out;
  ASSERT_TRUE(store.getXPath(bms[0], out));
  EXPECT_EQ(out, "/body/DocFragment[4]/body/p[42]");
  ASSERT_TRUE(store.getXPath(bms[1], out));
  EXPECT_EQ(out, "/body/DocFragment[6]/body/p[9]/text()[1].12");
}

// --- Adopting a peer's anchors out of the blob ------------------------------

namespace {
// A blob in the shape the device uploads: identity fields plus the resolved anchors.
std::string remoteBlobWithAnchors() {
  return R"({"b":[)"
         R"({"s":13,"p":0,"v":1,"pi":65535,"q":true,"sw":4,"ew":8,"sn":"thing we need",)"
         R"("xp":"/body/DocFragment[14]/body/p[1]/text()[1].10",)"
         R"("xp1":"/body/DocFragment[14]/body/p[1]/text()[1].32"},)"
         R"({"s":13,"p":0.068966,"v":9,"pi":5,"q":false,"sn":"Understanding",)"
         R"("xp":"/body/DocFragment[14]/body/p[5]"})"
         R"(],"t":[]})";
}
}  // namespace

TEST_F(BookmarkStoreTest, AdoptsAnchorsCarriedByARemoteBlob) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/adopt1.epub";

  // Exactly the sync order: nothing loaded, blob in hand.
  EXPECT_EQ(store.adoptRemoteXPaths(remoteBlobWithAnchors().c_str(), book, "epub"), 2u);

  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  ASSERT_TRUE(BookmarkStore::parseFromJson(remoteBlobWithAnchors().c_str(), bms, tombs));
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.mergeFrom(bms, tombs), 2u);

  const auto& merged = store.getBookmarks();
  ASSERT_EQ(merged.size(), 2u);
  std::string out;
  std::string end;
  ASSERT_TRUE(store.getXPath(merged[0], out, &end));
  EXPECT_EQ(out, "/body/DocFragment[14]/body/p[1]/text()[1].10");
  EXPECT_EQ(end, "/body/DocFragment[14]/body/p[1]/text()[1].32") << "a highlight keeps both ends";
  ASSERT_TRUE(store.getXPath(merged[1], out, &end));
  EXPECT_EQ(out, "/body/DocFragment[14]/body/p[5]");
  EXPECT_TRUE(end.empty()) << "a point bookmark has no end anchor";
}

// The whole point: what the device re-uploads must still carry the peer's anchors, or
// every other reader loses them the moment this one syncs.
TEST_F(BookmarkStoreTest, ReUploadsAPeerAnchorItCouldNotHaveDerived) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/adopt2.epub";
  ASSERT_GT(store.adoptRemoteXPaths(remoteBlobWithAnchors().c_str(), book, "epub"), 0u);

  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  ASSERT_TRUE(BookmarkStore::parseFromJson(remoteBlobWithAnchors().c_str(), bms, tombs));
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.mergeFrom(bms, tombs), 2u);

  const std::string out = store.serializeWithAnchors();
  EXPECT_NE(out.find("/body/DocFragment[14]/body/p[1]/text()[1].10"), std::string::npos);
  EXPECT_NE(out.find("/body/DocFragment[14]/body/p[5]"), std::string::npos);
}

// The sidecar is append-only, so a duplicate is dead weight that never stops growing.
TEST_F(BookmarkStoreTest, DoesNotReAppendAnAnchorItAlreadyHolds) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/adopt3.epub";
  const std::string blob = remoteBlobWithAnchors();

  ASSERT_EQ(store.adoptRemoteXPaths(blob.c_str(), book, "epub"), 2u);
  const auto sidecar = realPath(crcName(book, "epub", ".xpath"));
  const auto sizeAfterFirst = std::filesystem::file_size(sidecar);

  EXPECT_EQ(store.adoptRemoteXPaths(blob.c_str(), book, "epub"), 0u) << "already held";
  EXPECT_EQ(std::filesystem::file_size(sidecar), sizeAfterFirst);
}

TEST_F(BookmarkStoreTest, IgnoresRecordsThatCarryNoAnchor) {
  auto& store = BookmarkStore::getInstance();
  const char* blob = R"({"b":[{"s":1,"p":0.5,"v":1,"pi":3,"q":false,"sn":"no anchor here"}],"t":[]})";
  EXPECT_EQ(store.adoptRemoteXPaths(blob, "/books/adopt4.epub", "epub"), 0u);
  EXPECT_FALSE(std::filesystem::exists(realPath(crcName("/books/adopt4.epub", "epub", ".xpath"))))
      << "nothing to store, so no sidecar is created";
}

// A cut XPath resolves to nothing. Storing one would republish a broken anchor with this
// device's authority behind it, so an over-long one is refused outright.
TEST_F(BookmarkStoreTest, DropsAnOverlongAnchorRatherThanTruncatingIt) {
  auto& store = BookmarkStore::getInstance();
  const std::string tooLong(BOOKMARK_XPATH_MAX + 1, 'x');
  const std::string blob = R"({"b":[{"s":1,"p":0.5,"v":1,"pi":3,"q":false,"xp":")" + tooLong + R"("}],"t":[]})";
  EXPECT_EQ(store.adoptRemoteXPaths(blob.c_str(), "/books/adopt5.epub", "epub"), 0u);

  // One exactly at the cap is fine — the boundary is inclusive.
  const std::string atCap(BOOKMARK_XPATH_MAX, 'y');
  const std::string ok = R"({"b":[{"s":1,"p":0.5,"v":1,"pi":3,"q":false,"xp":")" + atCap + R"("}],"t":[]})";
  EXPECT_EQ(store.adoptRemoteXPaths(ok.c_str(), "/books/adopt6.epub", "epub"), 1u);
}

// Runs at a point in sync where nothing is loaded, but must not depend on that: the
// singleton is long-lived, and a stale load would otherwise silently disable adoption.
TEST_F(BookmarkStoreTest, AdoptsIndependentlyOfWhichBookIsLoaded) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/adopt7-other.epub", "T", "A", "epub"));

  EXPECT_EQ(BookmarkStore::adoptRemoteXPaths(remoteBlobWithAnchors().c_str(), "/books/adopt7.epub", "epub"), 2u);
  EXPECT_TRUE(std::filesystem::exists(realPath(crcName("/books/adopt7.epub", "epub", ".xpath"))))
      << "written for the book named, not the one loaded";
  EXPECT_FALSE(std::filesystem::exists(realPath(crcName("/books/adopt7-other.epub", "epub", ".xpath"))))
      << "the loaded book's sidecar is untouched";
}

// An anchor this device already resolved itself must win: it was derived against the copy
// actually on this device, whereas the peer's was derived against its own.
TEST_F(BookmarkStoreTest, KeepsALocallyResolvedAnchorOverThePeers) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/adopt8.epub";
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.addQuote(13, 0.0f, 4, 8, 10, "Ch", "thing we need"), BookmarkStore::AddResult::Added);
  ASSERT_TRUE(store.setXPath(store.getBookmarks()[0], "/local/anchor"));
  store.unload();

  EXPECT_EQ(store.adoptRemoteXPaths(remoteBlobWithAnchors().c_str(), book, "epub"), 1u)
      << "only the record this device had no anchor for";

  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  std::string out;
  ASSERT_TRUE(store.getXPath(store.getBookmarks()[0], out));
  EXPECT_EQ(out, "/local/anchor");
}

TEST_F(BookmarkStoreTest, AdoptingToleratesAnEmptyOrUnparseableBlob) {
  auto& store = BookmarkStore::getInstance();
  EXPECT_EQ(store.adoptRemoteXPaths("", "/books/adopt9.epub", "epub"), 0u);
  EXPECT_EQ(store.adoptRemoteXPaths(nullptr, "/books/adopt9.epub", "epub"), 0u);
  EXPECT_EQ(store.adoptRemoteXPaths("{not json", "/books/adopt9.epub", "epub"), 0u);
}

TEST_F(BookmarkStoreTest, XPathSurvivesBookmarkVectorReordering) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp2.epub", "T", "A", "epub"));
  // Added out of order: the store sorts, so index 0 is the spine-1 mark afterwards.
  ASSERT_EQ(store.addBookmark(9, 0.10f, 10, "Ch9", 5), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 5), BookmarkStore::AddResult::Added);

  const auto& bms = store.getBookmarks();
  ASSERT_EQ(bms[0].spineIndex, 1) << "sorted by spine";
  ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[2]/body/p[5]"));
  ASSERT_TRUE(store.setXPath(bms[1], "/body/DocFragment[10]/body/p[5]"));

  // Insert a mark that sorts between them — every index shifts.
  ASSERT_EQ(store.addBookmark(4, 0.10f, 10, "Ch4", 5), BookmarkStore::AddResult::Added);
  ASSERT_EQ(bms.size(), 3u);

  std::string out;
  ASSERT_TRUE(store.getXPath(bms[0], out));
  EXPECT_EQ(out, "/body/DocFragment[2]/body/p[5]") << "keyed by identity, not by position";
  ASSERT_TRUE(store.getXPath(bms[2], out));
  EXPECT_EQ(out, "/body/DocFragment[10]/body/p[5]");
  EXPECT_FALSE(store.getXPath(bms[1], out)) << "the newly inserted mark has no anchor yet";
  EXPECT_TRUE(out.empty());
}

TEST_F(BookmarkStoreTest, LaterXPathAppendSupersedesEarlier) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp3.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(2, 0.40f, 10, "Ch2", 7), BookmarkStore::AddResult::Added);
  const auto& bms = store.getBookmarks();

  ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[3]/body/p[1]"));
  ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[3]/body/div[2]/p[7]"));

  std::string out;
  ASSERT_TRUE(store.getXPath(bms[0], out));
  EXPECT_EQ(out, "/body/DocFragment[3]/body/div[2]/p[7]");
}

TEST_F(BookmarkStoreTest, DeletingABookmarkReclaimsItsXPathEntry) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp4.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 3), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(2, 0.20f, 10, "Ch2", 4), BookmarkStore::AddResult::Added);
  {
    const auto& bms = store.getBookmarks();
    ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[2]/body/p[3]"));
    ASSERT_TRUE(store.setXPath(bms[1], "/body/DocFragment[3]/body/p[4]"));
  }

  ASSERT_TRUE(store.removeBookmarkAt(0));

  const auto& bms = store.getBookmarks();
  ASSERT_EQ(bms.size(), 1u);
  std::string out;
  ASSERT_TRUE(store.getXPath(bms[0], out)) << "the surviving bookmark keeps its anchor";
  EXPECT_EQ(out, "/body/DocFragment[3]/body/p[4]");

  // The removed entry is gone from disk, not merely unmatched.
  Bookmark ghost{};
  ghost.spineIndex = 1;
  ghost.progress = 0.10f;
  ghost.paragraphIndex = 3;
  EXPECT_FALSE(store.getXPath(ghost, out));
}

TEST_F(BookmarkStoreTest, XPathIsTruncatedToTheCap) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp5.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 3), BookmarkStore::AddResult::Added);
  const auto& bms = store.getBookmarks();

  const std::string huge(BOOKMARK_XPATH_MAX + 50, 'x');
  ASSERT_TRUE(store.setXPath(bms[0], huge));

  std::string out;
  ASSERT_TRUE(store.getXPath(bms[0], out));
  EXPECT_EQ(out.size(), BOOKMARK_XPATH_MAX);
}

TEST_F(BookmarkStoreTest, ReportsNoXPathBeforeOneIsStored) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp6.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 3), BookmarkStore::AddResult::Added);

  std::string out = "stale";
  EXPECT_FALSE(store.getXPath(store.getBookmarks()[0], out)) << "no sidecar file exists yet";
  EXPECT_TRUE(out.empty());
}

TEST_F(BookmarkStoreTest, WhichHaveXPathsReportsAnchoredMarksInOnePass) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp7.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 3), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(2, 0.20f, 10, "Ch2", 4), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(3, 0.30f, 10, "Ch3", 5), BookmarkStore::AddResult::Added);

  bool has[MAX_BOOKMARKS];
  EXPECT_EQ(store.whichHaveXPaths(has, 3), 0u) << "no sidecar yet";
  EXPECT_FALSE(has[0]);
  EXPECT_FALSE(has[1]);
  EXPECT_FALSE(has[2]);

  const auto& bms = store.getBookmarks();
  ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[2]/body/p[3]"));
  ASSERT_TRUE(store.setXPath(bms[2], "/body/DocFragment[4]/body/p[5]"));

  EXPECT_EQ(store.whichHaveXPaths(has, 3), 2u);
  EXPECT_TRUE(has[0]);
  EXPECT_FALSE(has[1]) << "the un-anchored mark is the one a backfill must resolve";
  EXPECT_TRUE(has[2]);
}

TEST_F(BookmarkStoreTest, WhichHaveXPathsAgreesWithGetXPath) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp8.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 3), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(2, 0.20f, 1, 4, 10, "Ch2", "quoted"), BookmarkStore::AddResult::Added);
  const auto& bms = store.getBookmarks();
  ASSERT_TRUE(store.setXPath(bms[1], "/body/DocFragment[3]/body/p[2]/text()[1].5"));

  bool has[MAX_BOOKMARKS];
  store.whichHaveXPaths(has, bms.size());
  for (size_t i = 0; i < bms.size(); i++) {
    std::string out;
    EXPECT_EQ(has[i], store.getXPath(bms[i], out)) << "index " << i;
  }
}

TEST_F(BookmarkStoreTest, WhichHaveXPathsClearsEntriesPastTheBookmarkCount) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xp9.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 3), BookmarkStore::AddResult::Added);
  ASSERT_TRUE(store.setXPath(store.getBookmarks()[0], "/body/DocFragment[2]/body/p[3]"));

  bool has[8];
  for (bool& h : has) h = true;  // caller's array is not assumed zeroed
  EXPECT_EQ(store.whichHaveXPaths(has, 8), 1u);
  EXPECT_TRUE(has[0]);
  for (size_t i = 1; i < 8; i++) EXPECT_FALSE(has[i]) << "index " << i;
}

TEST_F(BookmarkStoreTest, StoresBothEndsOfAHighlightAnchor) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xpends.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 7), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(2, 0.40f, 3, 8, 10, "Ch2", "quoted text"), BookmarkStore::AddResult::Added);
  const auto& bms = store.getBookmarks();
  ASSERT_EQ(bms.size(), 2u);

  ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[2]/body/p[7]"));
  ASSERT_TRUE(store.setXPath(bms[1], "/body/DocFragment[3]/body/p[2]/text()[1].4",
                             "/body/DocFragment[3]/body/p[2]/text()[1].15"));

  std::string start;
  std::string end;
  ASSERT_TRUE(store.getXPath(bms[0], start, &end));
  EXPECT_EQ(start, "/body/DocFragment[2]/body/p[7]");
  EXPECT_TRUE(end.empty()) << "a point bookmark has no end anchor";

  ASSERT_TRUE(store.getXPath(bms[1], start, &end));
  EXPECT_EQ(start, "/body/DocFragment[3]/body/p[2]/text()[1].4");
  EXPECT_EQ(end, "/body/DocFragment[3]/body/p[2]/text()[1].15");

  // The end anchor is optional to the caller, and skipping it must not desync the scan.
  ASSERT_TRUE(store.getXPath(bms[0], start));
  EXPECT_EQ(start, "/body/DocFragment[2]/body/p[7]");
}

TEST_F(BookmarkStoreTest, CompactionPreservesBothEndsOfASurvivingHighlight) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xpcompact.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addQuote(2, 0.20f, 1, 4, 10, "Ch2", "doomed"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(2, 0.60f, 9, 12, 10, "Ch2", "survivor"), BookmarkStore::AddResult::Added);
  {
    const auto& bms = store.getBookmarks();
    ASSERT_EQ(bms.size(), 2u);
    ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[3]/body/p[1]/text()[1].0",
                               "/body/DocFragment[3]/body/p[1]/text()[1].6"));
    ASSERT_TRUE(store.setXPath(bms[1], "/body/DocFragment[3]/body/p[4]/text()[1].2",
                               "/body/DocFragment[3]/body/p[4]/text()[1].10"));
  }

  ASSERT_TRUE(store.removeQuoteByRange(2, 1, 4));

  const auto& bms = store.getBookmarks();
  ASSERT_EQ(bms.size(), 1u);
  std::string start;
  std::string end;
  ASSERT_TRUE(store.getXPath(bms[0], start, &end));
  EXPECT_EQ(start, "/body/DocFragment[3]/body/p[4]/text()[1].2");
  EXPECT_EQ(end, "/body/DocFragment[3]/body/p[4]/text()[1].10");
}

TEST_F(BookmarkStoreTest, SyncBlobCarriesTheKOReaderAnchors) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xpblob.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 7), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(2, 0.40f, 3, 8, 10, "Ch2", "quoted text"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(3, 0.70f, 10, "Ch3", 11), BookmarkStore::AddResult::Added);
  {
    const auto& bms = store.getBookmarks();
    ASSERT_EQ(bms.size(), 3u);
    ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[2]/body/p[7]"));
    ASSERT_TRUE(store.setXPath(bms[1], "/body/DocFragment[3]/body/p[2]/text()[1].4",
                               "/body/DocFragment[3]/body/p[2]/text()[1].15"));
    // bms[2] is deliberately left unanchored.
  }

  const std::string json = store.serializeWithAnchors();
  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, json), DeserializationError::Ok);
  JsonArray arr = doc["b"].as<JsonArray>();
  ASSERT_EQ(arr.size(), 3u);

  EXPECT_STREQ(arr[0]["xp"], "/body/DocFragment[2]/body/p[7]");
  EXPECT_FALSE(arr[0]["xp1"].is<const char*>()) << "a point bookmark has no end anchor";
  EXPECT_STREQ(arr[1]["xp"], "/body/DocFragment[3]/body/p[2]/text()[1].4");
  EXPECT_STREQ(arr[1]["xp1"], "/body/DocFragment[3]/body/p[2]/text()[1].15");
  EXPECT_FALSE(arr[2]["xp"].is<const char*>()) << "an unresolved mark is emitted without one";

  // Everything the unanchored blob carried is still there, so an older peer still reads it.
  EXPECT_EQ(arr[1]["s"].as<int>(), 2);
  EXPECT_TRUE(arr[1]["q"].as<bool>());
  EXPECT_EQ(arr[1]["sw"].as<int>(), 3);
}

TEST_F(BookmarkStoreTest, SyncBlobOmitsAnchorsWhenNoneAreStored) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xpnone.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 7), BookmarkStore::AddResult::Added);

  EXPECT_EQ(store.serializeWithAnchors(), BookmarkStore::serializeToJson(store.getBookmarks(), store.getTombstones()));
}

TEST_F(BookmarkStoreTest, ReturnMarksDoNotShiftTheAnchorAlignment) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xpreturn.epub", "T", "A", "epub"));
  // A return mark sorts between the two real marks and is skipped by the serializer, so the
  // blob's array indices no longer line up with the bookmark vector's.
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 7), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(2, 0.50f, 10, "Ch2", 9, nullptr, /*returnMark=*/true), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addBookmark(3, 0.70f, 10, "Ch3", 11), BookmarkStore::AddResult::Added);
  {
    const auto& bms = store.getBookmarks();
    ASSERT_EQ(bms.size(), 3u);
    ASSERT_TRUE(bms[1].returnMark);
    ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[2]/body/p[7]"));
    ASSERT_TRUE(store.setXPath(bms[2], "/body/DocFragment[4]/body/p[11]"));
  }

  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, store.serializeWithAnchors()), DeserializationError::Ok);
  JsonArray arr = doc["b"].as<JsonArray>();
  ASSERT_EQ(arr.size(), 2u) << "the return mark is never synced";
  EXPECT_STREQ(arr[0]["xp"], "/body/DocFragment[2]/body/p[7]");
  EXPECT_STREQ(arr[1]["xp"], "/body/DocFragment[4]/body/p[11]");
}

TEST_F(BookmarkStoreTest, DiscardsAnXPathSidecarFromAnotherFirmwareVersion) {
  auto& store = BookmarkStore::getInstance();
  ASSERT_TRUE(store.loadForBook("/books/xpver.epub", "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.10f, 10, "Ch1", 7), BookmarkStore::AddResult::Added);

  // Plant a sidecar whose version byte is not the current one, as an earlier build's would be.
  const std::string path = realPath(crcName("/books/xpver.epub", "epub", ".xpath"));
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  {
    std::ofstream f(path, std::ios::binary);
    const char stale[] = {0x01, 'j', 'u', 'n', 'k'};
    f.write(stale, sizeof(stale));
  }

  const auto& bms = store.getBookmarks();
  ASSERT_TRUE(store.setXPath(bms[0], "/body/DocFragment[2]/body/p[7]"));

  std::string out;
  ASSERT_TRUE(store.getXPath(bms[0], out)) << "the rewritten sidecar must be readable";
  EXPECT_EQ(out, "/body/DocFragment[2]/body/p[7]");
}

// ---- Marks a KOReader peer made -------------------------------------------------
//
// A KOReader client cannot derive CrossPoint's coordinates, so it keys its own marks by
// a hash of their XPointer, in a band this device's numbering never reaches, and sends a
// guessed position alongside a correct anchor. Adoption is the device re-filing them.

namespace {
// A blob as a KOReader peer sends it: synthetic identity, real anchor, approximate
// position. The paragraph index and word range sit in the reserved band.
std::string foreignBlob() {
  return R"({"b":[
    {"s":2,"p":0.10,"v":4,"ct":"Ch","pi":50000,"sn":"a bookmark made in KOReader","cp":0,"pc":0,
     "xp":"/body/DocFragment[3]/body/p[12]"},
    {"s":2,"p":0.10,"v":5,"ct":"Ch","pi":65535,"sn":"a highlight made in KOReader","cp":0,"pc":0,
     "q":true,"es":2,"ep":0.10,"sw":51000,"ew":52000,
     "xp":"/body/DocFragment[3]/body/p[14]/text()[1].0",
     "xp1":"/body/DocFragment[3]/body/p[14]/text()[1].20"}],"t":[]})";
}

// Load a book holding exactly the two foreign marks above, anchors in the sidecar.
void loadWithForeignMarks(BookmarkStore& store, const std::string& book) {
  ASSERT_GT(store.adoptRemoteXPaths(foreignBlob().c_str(), book, "epub"), 0u);
  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  ASSERT_TRUE(BookmarkStore::parseFromJson(foreignBlob().c_str(), bms, tombs));
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.mergeFrom(bms, tombs), 2u);
}

size_t indexOfQuote(const BookmarkStore& store) {
  const auto& bms = store.getBookmarks();
  for (size_t i = 0; i < bms.size(); i++) {
    if (bms[i].quote) return i;
  }
  return bms.size();
}
size_t indexOfPoint(const BookmarkStore& store) {
  const auto& bms = store.getBookmarks();
  for (size_t i = 0; i < bms.size(); i++) {
    if (!bms[i].quote) return i;
  }
  return bms.size();
}
}  // namespace

TEST_F(BookmarkStoreTest, RecognisesAPeerMarkByItsReservedIdentity) {
  Bookmark point{};
  point.paragraphIndex = 50000;
  EXPECT_TRUE(BookmarkStore::isForeignMark(point));

  Bookmark native{};
  native.paragraphIndex = 412;
  EXPECT_FALSE(BookmarkStore::isForeignMark(native));

  Bookmark unanchored{};
  unanchored.paragraphIndex = UINT16_MAX;
  EXPECT_FALSE(BookmarkStore::isForeignMark(unanchored)) << "no anchor is not a synthetic one";

  Bookmark quote{};
  quote.quote = true;
  quote.startWord = 51000;
  EXPECT_TRUE(BookmarkStore::isForeignMark(quote));
  quote.startWord = 14;
  EXPECT_FALSE(BookmarkStore::isForeignMark(quote));
}

TEST_F(BookmarkStoreTest, AdoptingAPointBookmarkRekeysItAndBuriesTheSyntheticSpot) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign1.epub";
  loadWithForeignMarks(store, book);

  const size_t i = indexOfPoint(store);
  ASSERT_LT(i, store.getBookmarks().size());
  ASSERT_TRUE(store.adoptForeignMark(i, 2, 0.625f, 37, "Chapter Three", "/body/DocFragment[3]/body/p[12]"));

  const auto& bms = store.getBookmarks();
  ASSERT_EQ(bms.size(), 2u) << "re-filed, not duplicated";
  const size_t p = indexOfPoint(store);
  EXPECT_EQ(bms[p].paragraphIndex, 37);
  EXPECT_FLOAT_EQ(bms[p].progress, 0.625f);
  EXPECT_STREQ(bms[p].chapterTitle, "Chapter Three");
  EXPECT_FALSE(BookmarkStore::isForeignMark(bms[p]));
  EXPECT_STREQ(bms[p].snippet, "a bookmark made in KOReader") << "the peer's text survives the move";

  // The synthetic spot must die, or the peer re-sends it on every sync.
  const auto& tombs = store.getTombstones();
  bool buried = false;
  for (const auto& t : tombs) {
    if (!t.quote && t.paragraphIndex == 50000) buried = true;
  }
  EXPECT_TRUE(buried);
}

TEST_F(BookmarkStoreTest, AnAdoptedBookmarkKeepsThePeersAnchor) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign2.epub";
  loadWithForeignMarks(store, book);

  ASSERT_TRUE(store.adoptForeignMark(indexOfPoint(store), 2, 0.5f, 37, nullptr, "/body/DocFragment[3]/body/p[12]"));

  // Without this the mark would go back out unanchored and every KOReader peer would
  // drop it, having nothing to place it from.
  std::string out;
  std::string end;
  ASSERT_TRUE(store.getXPath(store.getBookmarks()[indexOfPoint(store)], out, &end));
  EXPECT_EQ(out, "/body/DocFragment[3]/body/p[12]");
}

TEST_F(BookmarkStoreTest, AdoptingAHighlightMovesItWithoutChangingItsIdentity) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign3.epub";
  loadWithForeignMarks(store, book);

  const size_t i = indexOfQuote(store);
  const uint32_t before = store.getBookmarks()[i].version;
  ASSERT_TRUE(store.adoptForeignMark(i, 2, 0.8f, UINT16_MAX, "Chapter Three",
                                     "/body/DocFragment[3]/body/p[14]/text()[1].0",
                                     "/body/DocFragment[3]/body/p[14]/text()[1].20"));

  const size_t q = indexOfQuote(store);
  const auto& bm = store.getBookmarks()[q];
  EXPECT_FLOAT_EQ(bm.progress, 0.8f);
  EXPECT_FLOAT_EQ(bm.endProgress, 0.8f);
  // The word range is the identity, and no page-local index can be derived here, so it
  // must survive untouched -- the peer's own copy is keyed by it.
  EXPECT_EQ(bm.startWord, 51000);
  EXPECT_EQ(bm.endWord, 52000);
  // Adoption must NOT bump the version. Nothing here is news to a peer -- the identity
  // is unchanged and the page numbers describe this device's pagination -- while raising
  // it above the last version the peers published makes their deletes lose the tie-break.
  EXPECT_EQ(bm.version, before) << "adopting a quote tells a peer nothing, so it must not outrank one";

  for (const auto& t : store.getTombstones()) {
    EXPECT_FALSE(t.quote && t.startWord == 51000) << "the identity did not change, so nothing died";
  }
}

// The sequence a KOReader peer actually produces: it makes a highlight, this device picks
// it up and adopts it when the book is opened, and the peer then deletes it. The peer can
// only stamp its tombstone one above the highest version it has been shown, so any bump
// this device kept to itself would outrank the delete and resurrect the highlight.
TEST_F(BookmarkStoreTest, ADeleteFromAPeerSurvivesThisDeviceAdoptingTheHighlight) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign5.epub";
  loadWithForeignMarks(store, book);

  const size_t i = indexOfQuote(store);
  const uint32_t published = store.getBookmarks()[i].version;

  // Opening the book adopts it: page numbers and progress get filled in locally.
  ASSERT_TRUE(store.adoptForeignMark(i, 2, 0.8f, UINT16_MAX, "Chapter Three", "/xp", "/xp1"));

  // The peer saw only `published`, so this is the best it can stamp.
  const Tombstone del{2, UINT16_MAX, 0.8f, published + 1, true, 51000, 52000};
  store.mergeFrom({}, {del});

  for (const auto& b : store.getBookmarks()) {
    EXPECT_FALSE(b.quote && b.startWord == 51000) << "the peer's delete was undone by a local adoption";
  }
}

TEST_F(BookmarkStoreTest, AdoptingAHighlightTwiceIsANoOp) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign4.epub";
  loadWithForeignMarks(store, book);

  ASSERT_TRUE(store.adoptForeignMark(indexOfQuote(store), 2, 0.8f, UINT16_MAX, "Ch2", "/xp", "/xp1"));
  const uint32_t settled = store.getBookmarks()[indexOfQuote(store)].version;
  // Every sync re-resolves the same anchor; a version bump each time would be an
  // upload each time.
  EXPECT_FALSE(store.adoptForeignMark(indexOfQuote(store), 2, 0.8f, UINT16_MAX, "Ch2", "/xp", "/xp1"));
  EXPECT_EQ(store.getBookmarks()[indexOfQuote(store)].version, settled);
}

TEST_F(BookmarkStoreTest, RefusesToAdoptAMarkThisDeviceMadeItself) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign5.epub";
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.2f, 10, "Ch", 9, "mine"), BookmarkStore::AddResult::Added);

  EXPECT_FALSE(store.adoptForeignMark(0, 1, 0.9f, 44, "Ch", "/xp"));
  EXPECT_EQ(store.getBookmarks()[0].paragraphIndex, 9) << "a native mark is left alone";
}

TEST_F(BookmarkStoreTest, AdoptingOutOfRangeIsHarmless) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign6.epub";
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  EXPECT_FALSE(store.adoptForeignMark(0, 1, 0.5f, 3, "Ch", "/xp"));
  EXPECT_FALSE(store.adoptForeignMark(99, 1, 0.5f, 3, "Ch", "/xp"));
}

TEST_F(BookmarkStoreTest, AnAdoptedBookmarkSurvivesAReload) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign7.epub";
  loadWithForeignMarks(store, book);
  ASSERT_TRUE(store.adoptForeignMark(indexOfPoint(store), 2, 0.625f, 37, "Ch", "/body/DocFragment[3]/body/p[12]"));
  store.unload();

  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  const size_t p = indexOfPoint(store);
  ASSERT_LT(p, store.getBookmarks().size());
  EXPECT_EQ(store.getBookmarks()[p].paragraphIndex, 37);
  EXPECT_FALSE(BookmarkStore::isForeignMark(store.getBookmarks()[p]));
}

// The adopted pair has to reach the peers: a native record plus a tombstone for the
// synthetic spot is what makes the KOReader side re-key rather than duplicate.
TEST_F(BookmarkStoreTest, TheAdoptedPairGoesOutOnTheNextUpload) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign8.epub";
  loadWithForeignMarks(store, book);
  ASSERT_TRUE(store.adoptForeignMark(indexOfPoint(store), 2, 0.625f, 37, "Ch", "/body/DocFragment[3]/body/p[12]"));

  const std::string json = store.serializeWithAnchors();
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, json));

  bool nativeSent = false;
  for (JsonObject o : doc["b"].as<JsonArray>()) {
    if (o["pi"].as<uint16_t>() == 37) {
      nativeSent = true;
      EXPECT_STREQ(o["xp"].as<const char*>(), "/body/DocFragment[3]/body/p[12]");
    }
    EXPECT_NE(o["pi"].as<uint16_t>(), 50000) << "the synthetic record must not still be live";
  }
  EXPECT_TRUE(nativeSent);

  bool tombSent = false;
  for (JsonObject o : doc["t"].as<JsonArray>()) {
    if (o["pi"].as<uint16_t>() == 50000) tombSent = true;
  }
  EXPECT_TRUE(tombSent);
}

// Two peer bookmarks in one chapter whose anchors resolve to no paragraph: both end up
// keyed by progress alone, so a lookup that matched on the paragraph index would hand
// the second one's anchor to the first.
TEST_F(BookmarkStoreTest, AdoptingTwoUnparagraphedMarksKeepsTheirAnchorsApart) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/foreign9.epub";

  const std::string blob = R"({"b":[
    {"s":1,"p":0.10,"v":1,"ct":"Ch","pi":50000,"sn":"first","cp":0,"pc":0,
     "xp":"/body/DocFragment[2]/body/div[1]"},
    {"s":1,"p":0.20,"v":1,"ct":"Ch","pi":50001,"sn":"second","cp":0,"pc":0,
     "xp":"/body/DocFragment[2]/body/div[9]"}],"t":[]})";
  ASSERT_EQ(store.adoptRemoteXPaths(blob.c_str(), book, "epub"), 2u);
  std::vector<Bookmark> bms;
  std::vector<Tombstone> tombs;
  ASSERT_TRUE(BookmarkStore::parseFromJson(blob.c_str(), bms, tombs));
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.mergeFrom(bms, tombs), 2u);

  // Neither anchor named a paragraph, so both land on UINT16_MAX at distinct progress.
  size_t i = store.indexOfMark(false, 1, 50000, 0.10f, 0, 0);
  ASSERT_NE(i, SIZE_MAX);
  ASSERT_TRUE(store.adoptForeignMark(i, 1, 0.30f, UINT16_MAX, "Ch", "/body/DocFragment[2]/body/div[1]"));
  i = store.indexOfMark(false, 1, 50001, 0.20f, 0, 0);
  ASSERT_NE(i, SIZE_MAX);
  ASSERT_TRUE(store.adoptForeignMark(i, 1, 0.70f, UINT16_MAX, "Ch", "/body/DocFragment[2]/body/div[9]"));

  std::string out;
  size_t a = store.indexOfMark(false, 1, UINT16_MAX, 0.30f, 0, 0);
  size_t b = store.indexOfMark(false, 1, UINT16_MAX, 0.70f, 0, 0);
  ASSERT_NE(a, SIZE_MAX);
  ASSERT_NE(b, SIZE_MAX);
  ASSERT_TRUE(store.getXPath(store.getBookmarks()[a], out));
  EXPECT_EQ(out, "/body/DocFragment[2]/body/div[1]");
  ASSERT_TRUE(store.getXPath(store.getBookmarks()[b], out));
  EXPECT_EQ(out, "/body/DocFragment[2]/body/div[9]");
}

TEST_F(BookmarkStoreTest, IndexOfMarkFindsAMarkByItsMergeKey) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/indexof.epub";
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(3, 0.25f, 0, "Ch", 11, "point"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addQuote(3, 0.40f, 5, 9, 0, "Ch", "quoted"), BookmarkStore::AddResult::Added);

  const size_t p = store.indexOfMark(false, 3, 11, 0.25f, 0, 0);
  ASSERT_NE(p, SIZE_MAX);
  EXPECT_FALSE(store.getBookmarks()[p].quote);
  const size_t q = store.indexOfMark(true, 3, UINT16_MAX, 0.40f, 5, 9);
  ASSERT_NE(q, SIZE_MAX);
  EXPECT_TRUE(store.getBookmarks()[q].quote);
  EXPECT_EQ(store.indexOfMark(false, 3, 99, 0.99f, 0, 0), SIZE_MAX);
}

// Bookmark::progress is a page fraction everywhere it is written (EpubReaderActivity
// stores currentPage / pageCount) and PageMarks::drawForPage slices a page against it in
// that same unit. Adoption used to write the character fraction an XPath resolves to,
// which put a peer's highlight on whatever page happened to share the number -- so it
// opened in the wrong place and never drew. The caller does the conversion now; this
// pins the page numbers it passes through, which are also what the list shows.
TEST_F(BookmarkStoreTest, AdoptingAHighlightRecordsThePageItLandsOn) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/foreign-paged1.epub");
  const size_t i = indexOfQuote(store);
  ASSERT_LT(i, store.getBookmarks().size());

  // page 7 of 20 -- the page fraction the reader itself would have stored.
  ASSERT_TRUE(store.adoptForeignMark(i, 2, 7.0f / 20.0f, UINT16_MAX, "Chapter Three", "/body/DocFragment[3]/body/p[12]",
                                     "", 7, 20));
  const size_t q = indexOfQuote(store);
  const Bookmark& bm = store.getBookmarks()[q];
  EXPECT_FLOAT_EQ(bm.progress, 7.0f / 20.0f);
  EXPECT_FLOAT_EQ(bm.endProgress, 7.0f / 20.0f);
  EXPECT_EQ(bm.chapterCurrentPage, 7);
  EXPECT_EQ(bm.chapterPageCount, 20);
  // Still foreign: the word range is its identity and cannot be re-derived here.
  EXPECT_TRUE(BookmarkStore::isForeignMark(bm));
}

TEST_F(BookmarkStoreTest, AdoptingAPointBookmarkRecordsThePageItLandsOn) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/foreign-paged2.epub");
  const size_t i = indexOfPoint(store);
  ASSERT_LT(i, store.getBookmarks().size());

  ASSERT_TRUE(
      store.adoptForeignMark(i, 2, 7.0f / 20.0f, 37, "Chapter Three", "/body/DocFragment[3]/body/p[12]", "", 7, 20));
  const Bookmark& bm = store.getBookmarks()[indexOfPoint(store)];
  EXPECT_FLOAT_EQ(bm.progress, 7.0f / 20.0f);
  EXPECT_EQ(bm.chapterCurrentPage, 7);
  EXPECT_EQ(bm.chapterPageCount, 20);
  EXPECT_FALSE(BookmarkStore::isForeignMark(bm));
}

// Passing no page numbers must not wipe what a mark already carries.
TEST_F(BookmarkStoreTest, AdoptingWithoutPageNumbersLeavesTheRecordedOnes) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/foreign-paged3.epub");
  const size_t i = indexOfQuote(store);
  const uint16_t before = store.getBookmarks()[i].chapterPageCount;

  ASSERT_TRUE(store.adoptForeignMark(i, 2, 0.9f, UINT16_MAX, "Chapter Three", "/body/DocFragment[3]/body/p[12]"));
  const Bookmark& bm = store.getBookmarks()[indexOfQuote(store)];
  EXPECT_EQ(bm.chapterPageCount, before);
  EXPECT_FLOAT_EQ(bm.progress, 0.9f);
}

// The book-open adoption pass runs on every open, so what it considers pending decides
// whether opening a book costs a chapter stream per peer mark or nothing at all.
TEST_F(BookmarkStoreTest, AFreshlyPulledPeerMarkCountsAsUnplaced) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/unplaced1.epub");

  // A peer sends cp/pc as 0: it has no idea how this device paginates.
  EXPECT_EQ(store.countUnplacedForeign(), 2u);
  EXPECT_TRUE(BookmarkStore::isUnplacedForeign(store.getBookmarks()[indexOfQuote(store)]));
  EXPECT_TRUE(BookmarkStore::isUnplacedForeign(store.getBookmarks()[indexOfPoint(store)]));
}

TEST_F(BookmarkStoreTest, APlacedPeerMarkIsNotPendingAgain) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/unplaced2.epub");

  ASSERT_TRUE(store.adoptForeignMark(indexOfQuote(store), 2, 0.8f, UINT16_MAX, "Ch", "/xp", "/xp1", 13, 22));
  ASSERT_TRUE(store.adoptForeignMark(indexOfPoint(store), 2, 0.5f, 37, "Ch", "/xp2", "", 8, 22));

  // An adopted quote stays foreign for ever -- its synthetic word range is its identity --
  // so isForeignMark cannot be the test, or every open would re-stream its chapter.
  EXPECT_TRUE(BookmarkStore::isForeignMark(store.getBookmarks()[indexOfQuote(store)]));
  EXPECT_EQ(store.countUnplacedForeign(), 0u);
}

TEST_F(BookmarkStoreTest, AMarkThisDeviceMadeIsNeverPending) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/unplaced3.epub";
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.2f, 10, "Ch", 9, "mine"), BookmarkStore::AddResult::Added);

  EXPECT_EQ(store.countUnplacedForeign(), 0u);
}

// A peer's page routinely begins mid-paragraph, so the paragraph index alone lands on the
// page that paragraph STARTS on -- a page early whenever the paragraph spans pages, which
// is common on a smaller screen. The exact character offset is what closes that gap.
TEST_F(BookmarkStoreTest, AdoptingRecordsTheExactCharacterOffset) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/offset1.epub");

  ASSERT_TRUE(store.adoptForeignMark(indexOfPoint(store), 2, 0.5f, 37, "Ch", "/xp", "", 8, 22, 41277u));
  EXPECT_EQ(store.getBookmarks()[indexOfPoint(store)].visibleTextOffset, 41277u);

  ASSERT_TRUE(store.adoptForeignMark(indexOfQuote(store), 2, 0.8f, UINT16_MAX, "Ch", "/xp", "/xp1", 13, 22, 52001u));
  EXPECT_EQ(store.getBookmarks()[indexOfQuote(store)].visibleTextOffset, 52001u);
}

TEST_F(BookmarkStoreTest, TheCharacterOffsetSurvivesAReload) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/offset2.epub";
  loadWithForeignMarks(store, book);
  ASSERT_TRUE(store.adoptForeignMark(indexOfPoint(store), 2, 0.5f, 37, "Ch", "/xp", "", 8, 22, 41277u));
  store.unload();

  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  EXPECT_EQ(store.getBookmarks()[indexOfPoint(store)].visibleTextOffset, 41277u);
}

// A re-resolution that only moves the offset must still be applied, or the mark keeps a
// stale position that nothing else will correct.
TEST_F(BookmarkStoreTest, AChangedOffsetAloneIsNotANoOp) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/offset3.epub");

  ASSERT_TRUE(store.adoptForeignMark(indexOfQuote(store), 2, 0.8f, UINT16_MAX, "Ch", "/xp", "/xp1", 13, 22, 100u));
  EXPECT_FALSE(store.adoptForeignMark(indexOfQuote(store), 2, 0.8f, UINT16_MAX, "Ch", "/xp", "/xp1", 13, 22, 100u));
  EXPECT_TRUE(store.adoptForeignMark(indexOfQuote(store), 2, 0.8f, UINT16_MAX, "Ch", "/xp", "/xp1", 13, 22, 250u));
  EXPECT_EQ(store.getBookmarks()[indexOfQuote(store)].visibleTextOffset, 250u);
}

// Older files carry no offset; they must still load and fall back to the paragraph.
TEST_F(BookmarkStoreTest, AMarkWithNoOffsetReadsAsZero) {
  auto& store = BookmarkStore::getInstance();
  const std::string book = "/books/offset4.epub";
  ASSERT_TRUE(store.loadForBook(book, "T", "A", "epub"));
  ASSERT_EQ(store.addBookmark(1, 0.2f, 10, "Ch", 9, "mine"), BookmarkStore::AddResult::Added);
  EXPECT_EQ(store.getBookmarks()[0].visibleTextOffset, 0u);
}

// A peer selects against its own, larger page, so one highlight there routinely covers
// two or three here. Without a resolved end the quote is drawn on its start page only --
// and, because the snippet never matches whole there, not drawn at all.
TEST_F(BookmarkStoreTest, AdoptingAHighlightRecordsWhereItEnds) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/span1.epub");

  ASSERT_TRUE(store.adoptForeignMark(indexOfQuote(store), 2, 0.25f, UINT16_MAX, "Ch", "/xp", "/xp1", 5, 20, 900u,
                                     /*endProgress=*/0.35f));
  const Bookmark& bm = store.getBookmarks()[indexOfQuote(store)];
  EXPECT_FLOAT_EQ(bm.progress, 0.25f);
  EXPECT_FLOAT_EQ(bm.endProgress, 0.35f);
}

// The end anchor may not resolve (a different edition, or no page table for it). The mark
// then has to behave exactly as it did before ends were resolved: confined to one page.
TEST_F(BookmarkStoreTest, AnUnresolvedEndLeavesTheQuoteOnOnePage) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/span2.epub");

  ASSERT_TRUE(store.adoptForeignMark(indexOfQuote(store), 2, 0.25f, UINT16_MAX, "Ch", "/xp", "/xp1", 5, 20, 900u));
  const Bookmark& bm = store.getBookmarks()[indexOfQuote(store)];
  EXPECT_FLOAT_EQ(bm.endProgress, bm.progress);
}

TEST_F(BookmarkStoreTest, AChangedEndAloneIsNotANoOp) {
  auto& store = BookmarkStore::getInstance();
  loadWithForeignMarks(store, "/books/span3.epub");
  const size_t q = indexOfQuote(store);

  ASSERT_TRUE(store.adoptForeignMark(q, 2, 0.25f, UINT16_MAX, "Ch", "/xp", "/xp1", 5, 20, 900u, 0.35f));
  EXPECT_FALSE(
      store.adoptForeignMark(indexOfQuote(store), 2, 0.25f, UINT16_MAX, "Ch", "/xp", "/xp1", 5, 20, 900u, 0.35f));
  EXPECT_TRUE(
      store.adoptForeignMark(indexOfQuote(store), 2, 0.25f, UINT16_MAX, "Ch", "/xp", "/xp1", 5, 20, 900u, 0.45f));
  EXPECT_FLOAT_EQ(store.getBookmarks()[indexOfQuote(store)].endProgress, 0.45f);
}
