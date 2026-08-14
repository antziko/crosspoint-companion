#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "util/DictGloss.h"

// DictGloss feeds the reader's inline gloss box: three rows of a definition, refilled on every
// cursor move. Each test below covers one failure mode that has a visible consequence on the
// device rather than the happy path in several disguises:
//
//   fit()       - more content than three rows (the box must say so, not silently drop it)
//                 a CJK run, which is ONE space-less token and the reason the wrap has to
//                 break per codepoint instead of overflowing the row
//                 control bytes, which 'm' entries use as sense separators
//   readEntry() - a hit is NUL-terminated (the caller hands the buffer straight to
//                 prewarmCache, a C API)
//                 an entry longer than the buffer is cut on a codepoint boundary; cut
//                 mid-sequence it would reach the renderer as U+FFFD and draw as '?'
//                 a miss is 0, with no fallback probing (this path runs per keypress)

namespace {

namespace fs = std::filesystem;

// Deterministic stand-in for font metrics: every codepoint is 10 px wide, so a maxWidth of
// N*10 fits exactly N characters and the expected wrap is countable by hand. The device
// injects a real GfxRenderer-backed measurer through the same seam.
constexpr int kCpWidth = 10;

int measureByCodepoint(void*, const char* text, EpdFontFamily::Style, bool) {
  int cps = 0;
  for (const char* p = text; *p != '\0'; p++) {
    if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) cps++;
  }
  return cps * kCpWidth;
}

DictLayout::Measurer measurer() { return DictLayout::Measurer{nullptr, &measureByCodepoint}; }

DictLayout::WrapMetrics metricsForChars(int chars) { return DictLayout::WrapMetrics{chars * kCpWidth, 0, 0}; }

// Wrap a copy of `text`, so callers can pass literals to a function that edits in place.
void fitCopy(const std::string& text, int maxChars, DictGloss::GlossResult& out) {
  std::vector<char> buf(text.begin(), text.end());
  buf.push_back('\0');
  DictGloss::fit(buf.data(), metricsForChars(maxChars), measurer(), out);
}

std::string joinRows(const DictGloss::GlossResult& r) {
  std::string all;
  for (int i = 0; i < r.rowCount; i++) all += r.rows[i];
  return all;
}

class DictGlossTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            fs::path("cp-dict-gloss-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    fs::create_directories(root_);
    HalStorage::getInstance().setRoot(root_.string());
    // activeDictPath() prefers the session override, which would bypass the dictionary.bin the
    // fixtures below write.
    Dictionary::setSessionDictPath("");
  }

  void TearDown() override {
    HalStorage::getInstance().setRoot("");
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  // Copy a repo fixture dictionary under the temp root and point dictionary.bin at it — the
  // same shape the reader flow reads. Mirrors test/dict-lookup-session.
  void installFixture(const std::string& fixture, const std::string& deviceDir, const std::string& deviceBase) {
    const fs::path src = fs::path(REPO_ROOT) / "test" / "dictionaries" / fixture;
    ASSERT_TRUE(fs::exists(src)) << "missing fixture " << src;
    const fs::path dst = root_ / fs::path(deviceDir).relative_path();
    fs::create_directories(dst);
    for (const auto& e : fs::directory_iterator(src)) fs::copy_file(e.path(), dst / e.path().filename());
    writeDictBin(deviceBase);
  }

  // Minimal StarDict triple (.ifo/.idx/.dict) built in place. Exists because no repo fixture
  // carries a multi-KB CJK entry, and the codepoint-boundary cut can only be observed on one.
  // No .oft/.cspt: locate() falls back to a full scan, which is fine for a two-word index.
  void installSynthetic(const std::string& deviceDir, const std::string& base,
                        const std::vector<std::pair<std::string, std::string>>& entries) {
    const fs::path dst = root_ / fs::path(deviceDir).relative_path();
    fs::create_directories(dst);

    std::string idx;
    std::string dict;
    for (const auto& [word, body] : entries) {
      const uint32_t off = static_cast<uint32_t>(dict.size());
      const uint32_t len = static_cast<uint32_t>(body.size());
      idx += word;
      idx.push_back('\0');
      for (int shift = 24; shift >= 0; shift -= 8) idx.push_back(static_cast<char>((off >> shift) & 0xFF));
      for (int shift = 24; shift >= 0; shift -= 8) idx.push_back(static_cast<char>((len >> shift) & 0xFF));
      dict += body;
    }

    const std::string stem = base.substr(base.rfind('/') + 1);
    std::ofstream(dst / (stem + ".idx"), std::ios::binary).write(idx.data(), static_cast<long>(idx.size()));
    std::ofstream(dst / (stem + ".dict"), std::ios::binary).write(dict.data(), static_cast<long>(dict.size()));
    std::ofstream ifo(dst / (stem + ".ifo"), std::ios::binary);
    ifo << "StarDict's dict ifo file\nversion=2.4.2\nwordcount=" << entries.size() << "\nidxfilesize=" << idx.size()
        << "\nbookname=Synthetic\nsametypesequence=m\n";
    ifo.close();
    writeDictBin(base);
  }

  void writeDictBin(const std::string& contents) {
    const fs::path dir = root_ / ".crosspoint";
    fs::create_directories(dir);
    std::ofstream out(dir / "dictionary.bin", std::ios::binary);
    out << contents;
  }

  // Open the ctx + .dict handle pair the way DictionaryWordSelectActivity::initGloss does.
  void openSession(const std::string& deviceBase, Dictionary::LookupCtx& ctx, HalFile& dict) {
    ASSERT_TRUE(Dictionary::openLookupCtx(ctx, nullptr));
    ASSERT_TRUE(Storage.openFileForRead("TEST", deviceBase + ".dict", dict));
  }

  fs::path root_;
  static int counter_;
};

int DictGlossTest::counter_ = 0;

}  // namespace

// --- fit() -----------------------------------------------------------------------------

TEST_F(DictGlossTest, FitStopsAtThreeRowsAndSaysItTruncated) {
  DictGloss::GlossResult r;
  // 10 five-letter words at 5 chars per row-ish: far more than three rows can hold.
  fitCopy("alpha bravo charlie delta echo foxtrot golf hotel india juliet", 10, r);

  EXPECT_TRUE(r.found);
  EXPECT_EQ(r.rowCount, DictGloss::GlossResult::kMaxRows);
  EXPECT_TRUE(r.truncated);
  // The last row must advertise the cut. Three ASCII dots, not U+2026: no font on the card is
  // guaranteed to carry General Punctuation, and a missing glyph draws as '?'.
  const std::string last = r.rows[DictGloss::GlossResult::kMaxRows - 1];
  ASSERT_GE(last.size(), 3u);
  EXPECT_EQ(last.substr(last.size() - 3), "...");
}

TEST_F(DictGlossTest, FitBreaksSpacelessCjkRunPerCodepoint) {
  // 12 Han characters, no spaces anywhere: one token as far as any word-wrapper is concerned.
  // Without per-codepoint breaking this is a single row running off the box.
  const std::string cjk = "一二三四五六七八九十百千";
  DictGloss::GlossResult r;
  fitCopy(cjk, 5, r);

  EXPECT_TRUE(r.found);
  EXPECT_EQ(r.rowCount, 3);  // 12 characters at 5 per row = 3 rows, and the 3rd is short
  for (int i = 0; i < r.rowCount; i++) {
    const std::string row = r.rows[i];
    EXPECT_EQ(row.size() % 3, 0u) << "row " << i << " split a UTF-8 sequence: " << row;
    EXPECT_LE(measureByCodepoint(nullptr, r.rows[i], EpdFontFamily::REGULAR, false), metricsForChars(5).maxWidth);
  }
  // Nothing reordered or dropped in the rows that did fit.
  EXPECT_EQ(joinRows(r), cjk.substr(0, joinRows(r).size()));
}

TEST_F(DictGlossTest, FitCollapsesControlBytesAndDropsLeadingSpace) {
  // 'm' entries separate senses with newlines. With three rows visible, a flowing block shows
  // more than one sense per row would, so separators collapse to single spaces.
  DictGloss::GlossResult r;
  fitCopy("  noun:\ta tree\n\nverb: to climb  ", 40, r);

  ASSERT_EQ(r.rowCount, 1);
  EXPECT_STREQ(r.rows[0], "noun: a tree verb: to climb");
  EXPECT_FALSE(r.truncated);

  // An entry that is nothing but separators collapses to nothing, and must report a miss rather
  // than an empty box. cppcheck reads this branch as unreachable; it is not — the compaction
  // writes the terminator at the start of the buffer when no content byte ever arrives.
  fitCopy("\n \t\n", 40, r);
  EXPECT_FALSE(r.found);
  EXPECT_EQ(r.rowCount, 0);
}

TEST_F(DictGlossTest, FitMarksTheBracketedReadingBoldInACjkEntry) {
  // "拼音 [pin1 yin1] 定义" — the shape of a real 'm' Chinese entry: headword, bracketed reading,
  // definition. The box draws the reading bold so the pronunciation is separable from the meaning
  // at a glance while scanning. The reading is NOT at the start of the row, which is why the row
  // carries an offset as well as a length.
  DictGloss::GlossResult r;
  fitCopy("\xE6\x8B\xBC\xE9\x9F\xB3 [pin1 yin1] \xE5\xAE\x9A\xE4\xB9\x89", 60, r);

  ASSERT_EQ(r.rowCount, 1);
  // The row is drawn as three calls split at these offsets, so they have to bracket exactly the
  // "[...]" run — and land on codepoint boundaries, or a piece would end in a partial sequence
  // that renders as '?'.
  ASSERT_LE(r.boldStart[0] + r.boldLen[0], std::strlen(r.rows[0]));
  EXPECT_EQ(std::string(r.rows[0] + r.boldStart[0], r.boldLen[0]), "[pin1 yin1]");
}

TEST_F(DictGlossTest, ReadingIsUnmarkedWhenThereIsNoBracketedRunToMark) {
  // Every negative case resolves to "bold nothing", never "bold the wrong text".
  EXPECT_FALSE(DictGloss::findReading("noun: the round fruit of a tree.").found)
      << "an entry with no bracketed run has no reading to mark";
  EXPECT_FALSE(DictGloss::findReading("\xE6\x8B\xBC\xE9\x9F\xB3 [pin1 yin1").found)
      << "an unclosed bracket must not pair with some later ']'";
  EXPECT_FALSE(DictGloss::findReading("\xE6\x8B\xBC\xE9\x9F\xB3 []").found) << "empty brackets";
  EXPECT_FALSE(DictGloss::findReading(std::string(100, 'a').append("[pin1]").c_str()).found)
      << "a bracket this deep into the entry is definition text, not a reading";
  EXPECT_FALSE(DictGloss::findReading("head\nline2 [pin1]").found) << "first line only";

  DictGloss::GlossResult r;
  fitCopy("noun: a tree", 40, r);
  ASSERT_EQ(r.rowCount, 1);
  EXPECT_EQ(r.boldLen[0], 0);
}

// --- readEntry() -----------------------------------------------------------------------

TEST_F(DictGlossTest, ReadEntryReturnsNulTerminatedHit) {
  installFixture("english-full", "/dictionaries/english-full", "/dictionaries/english-full/english-full");
  Dictionary::LookupCtx ctx;
  HalFile dict;
  openSession("/dictionaries/english-full/english-full", ctx, dict);

  char buf[DictGloss::kPeekBytes];
  const size_t n = DictGloss::readEntry(ctx, dict, "apple", buf, sizeof(buf));

  ASSERT_GT(n, 0u);
  EXPECT_EQ(buf[n], '\0') << "the caller hands this straight to prewarmCache, a C API";
  EXPECT_STREQ(buf, "noun: the round fruit of a tree of the rose family.");
}

TEST_F(DictGlossTest, ReadEntryMissIsZeroWithNoFallback) {
  installFixture("english-full", "/dictionaries/english-full", "/dictionaries/english-full/english-full");
  Dictionary::LookupCtx ctx;
  HalFile dict;
  openSession("/dictionaries/english-full/english-full", ctx, dict);

  char buf[DictGloss::kPeekBytes];
  // "apples" is a real .syn alt form in this fixture, so this also pins that the peek does NOT
  // walk the stem/alt-form chain the full lookup does — every fallback is another SD probe on a
  // path that runs on every cursor move.
  EXPECT_EQ(DictGloss::readEntry(ctx, dict, "apples", buf, sizeof(buf)), 0u);
  EXPECT_EQ(DictGloss::readEntry(ctx, dict, "zzzznotaword", buf, sizeof(buf)), 0u);
}

TEST_F(DictGlossTest, ReadEntryCapsLongCjkEntryOnACodepointBoundary) {
  // 400 Han characters = 1200 bytes, well past kPeekBytes, and 512 is NOT a multiple of 3 — so
  // an unguarded cap would slice a 3-byte sequence and the last glyph would render as '?'.
  std::string body;
  for (int i = 0; i < 400; i++) body += "漢";
  installSynthetic("/dictionaries/st-cjk", "/dictionaries/st-cjk/st-cjk", {{"漢", body}});

  Dictionary::LookupCtx ctx;
  HalFile dict;
  openSession("/dictionaries/st-cjk/st-cjk", ctx, dict);

  char buf[DictGloss::kPeekBytes];
  const size_t n = DictGloss::readEntry(ctx, dict, "漢", buf, sizeof(buf));

  ASSERT_GT(n, 0u);
  EXPECT_LE(n, sizeof(buf) - 1);
  EXPECT_EQ(n % 3, 0u) << "cut mid-sequence: " << n << " bytes";
  EXPECT_EQ(buf[n], '\0');
  // And the wrap on top of it still yields three clean rows.
  DictGloss::GlossResult r;
  DictGloss::fit(buf, metricsForChars(6), measurer(), r);
  EXPECT_EQ(r.rowCount, DictGloss::GlossResult::kMaxRows);
  EXPECT_TRUE(r.truncated);
}
