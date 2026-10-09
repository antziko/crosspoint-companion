#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "RecentBooksStore.h"

namespace {
const char* kHistory = "/.crosspoint/recent.tsv";

class RecentBooksStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/recent-books-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    HalStorage::root() = tmpl;
    ASSERT_EQ(mkdir((HalStorage::root() + "/.crosspoint").c_str(), 0755), 0);
    ASSERT_TRUE(RECENT_BOOKS.loadFromFile());
  }
  void TearDown() override { std::system(("rm -rf '" + HalStorage::root() + "'").c_str()); }

  static std::string read(const char* path) {
    std::ifstream in(HalStorage::real(path), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
  static void write(const char* path, const std::string& text) {
    std::ofstream(HalStorage::real(path), std::ios::binary) << text;
  }
  static void add(const std::string& path) { RECENT_BOOKS.addBook(path, "T " + path, "A", "/c" + path); }

  // Every path in file order, via scan(), each confirmed by readAt() at its offset.
  static std::vector<std::string> all() {
    struct Ctx {
      std::vector<std::string> paths;
      std::vector<uint32_t> offsets;
    } ctx;
    RECENT_BOOKS.scan(
        [](void* raw, std::string_view path, uint32_t offset) {
          auto& c = *static_cast<Ctx*>(raw);
          c.paths.emplace_back(path);
          c.offsets.push_back(offset);
          return true;
        },
        &ctx);
    for (size_t i = 0; i < ctx.paths.size(); i++) {
      RecentBook book;
      EXPECT_TRUE(RECENT_BOOKS.readAt(ctx.offsets[i], book));
      EXPECT_EQ(book.path, ctx.paths[i]);
    }
    return ctx.paths;
  }
  static std::vector<std::string> headPaths() {
    std::vector<std::string> out;
    for (const auto& book : RECENT_BOOKS.getBooks()) out.push_back(book.path);
    return out;
  }
};
}  // namespace

TEST_F(RecentBooksStoreTest, AddPutsNewestFirstAndMovesExisting) {
  add("/a.epub");
  add("/b.epub");
  add("/c.epub");
  add("/a.epub");
  EXPECT_EQ(all(), (std::vector<std::string>{"/a.epub", "/c.epub", "/b.epub"}));
  EXPECT_EQ(headPaths(), all());
  ASSERT_TRUE(RECENT_BOOKS.loadFromFile());  // the head mirror matches a fresh read
  EXPECT_EQ(headPaths(), all());
  RecentBook front = RECENT_BOOKS.getBooks().front();
  EXPECT_EQ(front.title, "T /a.epub");
  EXPECT_EQ(front.coverBmpPath, "/c/a.epub");
}

TEST_F(RecentBooksStoreTest, CapsHistoryAndHead) {
  for (int i = 0; i < RecentBooksStore::MAX_RECENT_BOOKS + 5; i++) add("/b" + std::to_string(i) + ".epub");
  const auto paths = all();
  ASSERT_EQ(paths.size(), static_cast<size_t>(RecentBooksStore::MAX_RECENT_BOOKS));
  EXPECT_EQ(paths.front(), "/b504.epub");
  EXPECT_EQ(paths.back(), "/b5.epub");
  EXPECT_EQ(RECENT_BOOKS.getBooks().size(), static_cast<size_t>(RecentBooksStore::HEAD_COUNT));
}

TEST_F(RecentBooksStoreTest, RemoveByPathAndUnderFolder) {
  for (const char* p : {"/calibre/x.epub", "/calibre2/y.epub", "/calibre/sub/z.epub", "/root.epub"}) add(p);
  EXPECT_FALSE(RECENT_BOOKS.removeByPath("/missing.epub"));
  EXPECT_TRUE(RECENT_BOOKS.removeByPath("/root.epub"));
  EXPECT_TRUE(RECENT_BOOKS.removeUnder("/calibre"));
  EXPECT_EQ(all(), (std::vector<std::string>{"/calibre2/y.epub"}));
  EXPECT_EQ(headPaths(), all());
  EXPECT_FALSE(RECENT_BOOKS.removeUnder("/calibre"));
}

TEST_F(RecentBooksStoreTest, SwapKeepsBooksBetween) {
  for (const char* p : {"/d.epub", "/c.epub", "/b.epub", "/a.epub"}) add(p);  // a b c d
  EXPECT_TRUE(RECENT_BOOKS.swapEntries("/a.epub", "/d.epub"));
  EXPECT_EQ(all(), (std::vector<std::string>{"/d.epub", "/b.epub", "/c.epub", "/a.epub"}));
  EXPECT_EQ(RECENT_BOOKS.getBooks().front().title, "T /d.epub");
  EXPECT_FALSE(RECENT_BOOKS.swapEntries("/a.epub", "/missing.epub"));
}

TEST_F(RecentBooksStoreTest, UpdatePathKeepsPositionAndRemapsCover) {
  RECENT_BOOKS.addBook("/old/b.epub", "B", "A", "/.crosspoint/epub_1/cover.bmp");
  add("/a.epub");
  RECENT_BOOKS.updatePath("/old/b.epub", "/new/b.epub", "/.crosspoint/epub_1", "/.crosspoint/epub_2");
  EXPECT_EQ(all(), (std::vector<std::string>{"/a.epub", "/new/b.epub"}));
  EXPECT_EQ(RECENT_BOOKS.getBooks()[1].coverBmpPath, "/.crosspoint/epub_2/cover.bmp");
}

TEST_F(RecentBooksStoreTest, ConvertsLegacyJsonOnce) {
  std::remove(HalStorage::real(kHistory).c_str());
  write("/.crosspoint/recent.json",
        R"({"books":[{"path":"/one.epub","title":"One","author":"X","coverBmpPath":"/c1"},)"
        R"({"path":"/two.epub","title":"Tab\there","author":"","coverBmpPath":""},{"path":""}]})");
  ASSERT_TRUE(RECENT_BOOKS.loadFromFile());
  EXPECT_EQ(all(), (std::vector<std::string>{"/one.epub", "/two.epub"}));
  EXPECT_EQ(RECENT_BOOKS.getBooks()[1].title, "Tab here");
  EXPECT_FALSE(Storage.exists("/.crosspoint/recent.json"));
}

TEST_F(RecentBooksStoreTest, SkipsOverlongAndBlankLines) {
  write(kHistory, "/a.epub\tA\t\t\n\n/" + std::string(3000, 'x') + "\tLong\t\t\n/b.epub\tB\t\t\n");
  ASSERT_TRUE(RECENT_BOOKS.loadFromFile());
  EXPECT_EQ(all(), (std::vector<std::string>{"/a.epub", "/b.epub"}));
  add("/c.epub");  // a rewrite drops the corrupt line
  EXPECT_EQ(read(kHistory), "/c.epub\tT /c.epub\tA\t/c/c.epub\n/a.epub\tA\t\t\n/b.epub\tB\t\t\n");
}
