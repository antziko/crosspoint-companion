#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "util/Dictionary.h"

// Dictionary::LookupCtx exists so a probe sequence opens its files once instead of once per
// probe. Before it, every Dictionary::locate() call re-opened four files: dictionary.bin (via
// activeDictPath), .idx, the page index inside resolveScanBounds, and the SAME page index
// again inside the widened-retry path taken on every miss. The stem-variant fallback in
// DictionaryLookupController runs up to six probes, so a missed word cost ~28 SD opens and
// ~42 transient std::strings before findSimilar() even started.
//
// That is invisible in results — locateIn() and locate() return identical values — so the
// only thing keeping the optimisation alive is the open count. Both are asserted here:
// equivalence (so the refactor cannot change behaviour) and open counts (so it cannot rot).

namespace {

namespace fs = std::filesystem;

// Device-absolute base path the tests point dictionary.bin at. The stub splices device paths
// under a temp root, so this never touches a real /dictionaries.
constexpr char kDictDir[] = "/dictionaries/english-full";
constexpr char kDictBase[] = "/dictionaries/english-full/english-full";

class DictLookupSession : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() /
            fs::path("cp-dict-session-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    fs::create_directories(root_);
    HalStorage::getInstance().setRoot(root_.string());
    // Session override leaks across tests otherwise: activeDictPath() prefers it over
    // dictionary.bin, which would silently skip the path-resolution open being counted.
    Dictionary::setSessionDictPath("");
  }

  void TearDown() override {
    Dictionary::setSessionDictPath("");
    HalStorage::getInstance().setRoot("");
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  // Copy a fixture dictionary under the temp root at the given device path, and point the
  // global dictionary.bin at it — the same shape the reader flow reads.
  void installDictionary(const std::string& fixture, const std::string& deviceDir, const std::string& deviceBase) {
    const fs::path src = fs::path(REPO_ROOT) / "test" / "dictionaries" / fixture;
    ASSERT_TRUE(fs::exists(src)) << "missing fixture " << src;
    const fs::path dst = root_ / fs::path(deviceDir).relative_path();
    fs::create_directories(dst);
    for (const auto& e : fs::directory_iterator(src)) fs::copy_file(e.path(), dst / e.path().filename());
    writeDictBin(deviceBase);
  }

  void writeDictBin(const std::string& contents) {
    const fs::path dir = root_ / ".crosspoint";
    fs::create_directories(dir);
    std::ofstream out(dir / "dictionary.bin", std::ios::binary);
    out << contents;
  }

  static int opens() { return HalStorage::getInstance().readOpenCount; }
  static void resetOpens() { HalStorage::getInstance().resetOpenCount(); }

  fs::path root_;
  static int counter_;
};

int DictLookupSession::counter_ = 0;

}  // namespace

// --- Baseline: the refactored locate() still finds words ----------------------

TEST_F(DictLookupSession, LocateFindsDirectHit) {
  installDictionary("english-full", kDictDir, kDictBase);

  const auto loc = Dictionary::locate("apple");
  EXPECT_TRUE(loc.found);
  EXPECT_EQ(loc.folderPath, kDictBase);
  EXPECT_GT(loc.size, 0u);
}

TEST_F(DictLookupSession, LocateMissesAbsentWord) {
  installDictionary("english-full", kDictDir, kDictBase);

  EXPECT_FALSE(Dictionary::locate("notinthisdictionary").found);
}

TEST_F(DictLookupSession, LocateIsCaseInsensitive) {
  installDictionary("english-full", kDictDir, kDictBase);

  const auto lower = Dictionary::locate("grove");
  const auto upper = Dictionary::locate("GROVE");
  ASSERT_TRUE(lower.found);
  ASSERT_TRUE(upper.found);
  EXPECT_EQ(lower.offset, upper.offset);
  EXPECT_EQ(lower.size, upper.size);
}

// --- Equivalence: locateIn() must match locate() exactly ----------------------

TEST_F(DictLookupSession, LocateInMatchesLocateAcrossHitsAndMisses) {
  installDictionary("english-full", kDictDir, kDictBase);

  // Deliberately mixes hits, misses, a hyphenated headword, first and last entries, and
  // words either side of a page boundary — the cases where a bounds bug would show.
  const std::vector<std::string> words{"above-mentioned", "apple",  "grape",   "graze", "grove",
                                       "zenith",          "aaaaaa", "zzzzzz",  "grad",  "grave",
                                       "mother-in-law",   "xenon",  "notaword"};

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));

  for (const auto& w : words) {
    const auto viaLocate = Dictionary::locate(w);
    const auto viaCtx = Dictionary::locateIn(ctx, w);
    EXPECT_EQ(viaLocate.found, viaCtx.found) << "found mismatch for '" << w << "'";
    EXPECT_EQ(viaLocate.offset, viaCtx.offset) << "offset mismatch for '" << w << "'";
    EXPECT_EQ(viaLocate.size, viaCtx.size) << "size mismatch for '" << w << "'";
    EXPECT_EQ(viaLocate.folderPath, viaCtx.folderPath) << "folderPath mismatch for '" << w << "'";
  }
}

TEST_F(DictLookupSession, CtxIsReusableAfterAHit) {
  installDictionary("english-full", kDictDir, kDictBase);

  // A hit used to close the .idx handle. Reusing the ctx afterwards must still work, or the
  // stem loop would silently stop probing after its first success.
  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));

  EXPECT_TRUE(Dictionary::locateIn(ctx, "apple").found);
  EXPECT_TRUE(Dictionary::locateIn(ctx, "zenith").found);
  EXPECT_FALSE(Dictionary::locateIn(ctx, "notaword").found);
  EXPECT_TRUE(Dictionary::locateIn(ctx, "cloud").found);
}

// --- The regression guard: open counts ---------------------------------------

TEST_F(DictLookupSession, ProbeSequenceOpensFilesOnce) {
  installDictionary("english-full", kDictDir, kDictBase);

  // Six misses is what a stem-variant fallback costs on a word that resolves to nothing.
  const std::vector<std::string> probes{"miss1", "miss2", "miss3", "miss4", "miss5", "miss6"};

  resetOpens();
  for (const auto& w : probes) (void)Dictionary::locate(w);
  const int perCallOpens = opens();

  Dictionary::LookupCtx ctx;
  resetOpens();
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  for (const auto& w : probes) (void)Dictionary::locateIn(ctx, w);
  const int sessionOpens = opens();

  // The session opens dictionary.bin, .idx and the page index once, and never again.
  EXPECT_EQ(sessionOpens, 3);
  // Per-call scales with the probe count; the session does not. Assert the shape, not a
  // magic number, so the test survives a change in how many files a probe consults.
  EXPECT_GT(perCallOpens, sessionOpens * 3);
  EXPECT_GE(perCallOpens, static_cast<int>(probes.size()) * 3);
}

TEST_F(DictLookupSession, SessionOpenCountDoesNotGrowWithProbes) {
  installDictionary("english-full", kDictDir, kDictBase);

  Dictionary::LookupCtx ctx;
  resetOpens();
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  const int afterOpen = opens();

  for (int i = 0; i < 25; ++i) (void)Dictionary::locateIn(ctx, "miss" + std::to_string(i));
  EXPECT_EQ(opens(), afterOpen) << "locateIn must not open any file";

  (void)Dictionary::locateIn(ctx, "apple");
  EXPECT_EQ(opens(), afterOpen) << "a hit must not open any file either";
}

// --- Degraded dictionaries ----------------------------------------------------

TEST_F(DictLookupSession, WorksWithoutAPageIndex) {
  // no-ifo ships .idx and .dict only. Without .cspt/.oft, locate() falls back to a full scan
  // and must still find words — the ctx just carries no page index.
  installDictionary("no-ifo", "/dictionaries/no-ifo", "/dictionaries/no-ifo/no-ifo");

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  EXPECT_FALSE(ctx.hasPageIndex);

  EXPECT_TRUE(Dictionary::locateIn(ctx, "apple").found);
  EXPECT_TRUE(Dictionary::locateIn(ctx, "echo").found);
  EXPECT_FALSE(Dictionary::locateIn(ctx, "notaword").found);
}

TEST_F(DictLookupSession, StaleCsptFallsBackToOftNotAFullScan) {
  // A .cspt whose magic no longer validates (the shape of a format-version bump) must fall
  // back to .oft, exactly as the path-based resolveScanBounds does. Losing that fallback is
  // invisible in results — every word is still found by full scan — but on a large
  // dictionary it turns every probe into a whole-file read, so assert the .oft actually gets
  // opened rather than trusting the answer.
  installDictionary("english-full", kDictDir, kDictBase);
  const fs::path cspt = root_ / "dictionaries/english-full/english-full.idx.oft.cspt";
  ASSERT_TRUE(fs::exists(cspt));
  {
    std::fstream f(cspt, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f.seekp(0);
    f.write("XXXX", 4);  // clobber the magic
  }

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  ASSERT_TRUE(ctx.pageIndexIsCspt);

  resetOpens();
  EXPECT_TRUE(Dictionary::locateIn(ctx, "grove").found);
  EXPECT_EQ(opens(), 1) << ".oft fallback should be opened exactly once, lazily";
  EXPECT_TRUE(ctx.hasOftFallback);

  // And only once: the second probe reuses the handle opened by the first.
  resetOpens();
  EXPECT_TRUE(Dictionary::locateIn(ctx, "apple").found);
  EXPECT_TRUE(Dictionary::locateIn(ctx, "zenith").found);
  EXPECT_EQ(opens(), 0);
}

TEST_F(DictLookupSession, StaleCsptMissStillResolvesEveryWord) {
  // The miss path widens the scan window using a page index. With a stale .cspt it must
  // widen against the .oft fallback, not against the sidecar already known to be bad —
  // widening off garbage page starts sends the retry to the wrong region of the .idx.
  // Every headword must still be findable, including across the whole file.
  installDictionary("english-full", kDictDir, kDictBase);
  const fs::path cspt = root_ / "dictionaries/english-full/english-full.idx.oft.cspt";
  {
    std::fstream f(cspt, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f.seekp(0);
    f.write("XXXX", 4);
  }

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  for (const char* w : {"above-mentioned", "apple", "grove", "point", "willow", "zenith"}) {
    EXPECT_TRUE(Dictionary::locateIn(ctx, w).found) << w;
  }
  EXPECT_FALSE(Dictionary::locateIn(ctx, "notaword").found);
}

TEST_F(DictLookupSession, ReopeningACtxAgainstAnotherDictionaryDoesNotLeakState) {
  // Reopening a ctx must fully reset it. The lazily-opened .oft fallback is the dangerous
  // one: left set, the second dictionary's probes would resolve their scan bounds against
  // the FIRST dictionary's page index and answer from the wrong region.
  installDictionary("english-full", kDictDir, kDictBase);
  const fs::path cspt = root_ / "dictionaries/english-full/english-full.idx.oft.cspt";
  {
    std::fstream f(cspt, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f.seekp(0);
    f.write("XXXX", 4);
  }

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  EXPECT_TRUE(Dictionary::locateIn(ctx, "grove").found);
  ASSERT_TRUE(ctx.hasOftFallback) << "precondition: the stale .cspt forced the fallback open";

  // Point the same ctx at a different dictionary that has no page index at all.
  installDictionary("no-ifo", "/dictionaries/no-ifo", "/dictionaries/no-ifo/no-ifo");
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  EXPECT_FALSE(ctx.hasOftFallback);
  EXPECT_FALSE(ctx.hasPageIndex);
  EXPECT_FALSE(ctx.pageIndexIsCspt);
  EXPECT_STREQ(ctx.base, "/dictionaries/no-ifo/no-ifo");

  const auto loc = Dictionary::locateIn(ctx, "apple");
  EXPECT_TRUE(loc.found);
  EXPECT_EQ(loc.folderPath, "/dictionaries/no-ifo/no-ifo");
}

TEST_F(DictLookupSession, NoDictionaryConfigured) {
  // No dictionary.bin at all.
  Dictionary::LookupCtx ctx;
  EXPECT_FALSE(Dictionary::openLookupCtx(ctx));
  EXPECT_FALSE(ctx.valid);
  EXPECT_STREQ(ctx.base, "");

  const auto loc = Dictionary::locate("apple");
  EXPECT_FALSE(loc.found);
  EXPECT_TRUE(loc.folderPath.empty());
}

TEST_F(DictLookupSession, DictionaryConfiguredButIdxMissing) {
  // dictionary.bin points somewhere real that has no .idx: distinct from "not configured",
  // and ctx.base is still filled so a caller can tell the two apart.
  installDictionary("only-dict", "/dictionaries/only-dict", "/dictionaries/only-dict/only-dict");

  Dictionary::LookupCtx ctx;
  EXPECT_FALSE(Dictionary::openLookupCtx(ctx));
  EXPECT_FALSE(ctx.valid);
  EXPECT_STREQ(ctx.base, "/dictionaries/only-dict/only-dict");

  EXPECT_FALSE(Dictionary::locate("apple").found);
}

TEST_F(DictLookupSession, LocateInOnUnopenedCtxIsSafe) {
  installDictionary("english-full", kDictDir, kDictBase);

  Dictionary::LookupCtx ctx;  // never passed to openLookupCtx
  const auto loc = Dictionary::locateIn(ctx, "apple");
  EXPECT_FALSE(loc.found);
}

// Pins the HalFile contract this stub must keep mirroring. openLookupCtx once reset by calling
// close() on all three handles; on a fresh stack LookupCtx those have no pimpl, and close()
// asserted impl != nullptr, so every lookup panicked on device with
// "assert failed: bool HalFile::close()". close() is now a no-op on a null impl (HalStorage.cpp)
// — but the ctx still releases by move-assignment, which additionally frees the Impl.
TEST_F(DictLookupSession, ClosingANeverOpenedHandleIsANoOp) {
  HalFile f;
  EXPECT_TRUE(f.close());
  EXPECT_FALSE(f.isOpen());
}

TEST_F(DictLookupSession, OpeningAFreshCtxTwiceIsSafe) {
  installDictionary("english-full", kDictDir, kDictBase);

  Dictionary::LookupCtx ctx;  // all three HalFiles have no impl yet
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  // .oftFallback is opened lazily, so it is still impl-less on the second pass.
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  EXPECT_TRUE(Dictionary::locateIn(ctx, "apple").found);
}

// --- Failure reason -----------------------------------------------------------

TEST_F(DictLookupSession, StatusSeparatesMissFromBrokenDictionary) {
  // The three outcomes must be distinguishable, because the UI reports them differently:
  // a miss offers suggestions, the other two say what is actually wrong instead of
  // "Not found" — which reads as "you spelled it wrong".
  installDictionary("english-full", kDictDir, kDictBase);
  EXPECT_EQ(Dictionary::locate("apple").status, LookupStatus::Found);
  EXPECT_EQ(Dictionary::locate("notaword").status, LookupStatus::NotFound);

  // Configured, but the .idx is gone.
  fs::remove(root_ / "dictionaries/english-full/english-full.idx");
  EXPECT_EQ(Dictionary::locate("apple").status, LookupStatus::ReadError);

  // Nothing configured at all.
  fs::remove(root_ / ".crosspoint/dictionary.bin");
  EXPECT_EQ(Dictionary::locate("apple").status, LookupStatus::NoDictionary);
}

TEST_F(DictLookupSession, StatusIsSetOnTheWidenedRetryHitToo) {
  // The widened-retry path has its own `found = true` assignment; a status left at the
  // default there would report a hit as NotFound to the caller.
  installDictionary("english-full", kDictDir, kDictBase);

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  for (const char* w : {"apple", "grove", "zenith", "above-mentioned"}) {
    const auto loc = Dictionary::locateIn(ctx, w);
    ASSERT_TRUE(loc.found) << w;
    EXPECT_EQ(loc.status, LookupStatus::Found) << w;
  }
}

TEST_F(DictLookupSession, StatusFromLocateInMatchesLocate) {
  installDictionary("english-full", kDictDir, kDictBase);

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  for (const char* w : {"apple", "notaword", "zzzzzz", "grave"}) {
    EXPECT_EQ(Dictionary::locate(w).status, Dictionary::locateIn(ctx, w).status) << w;
  }
}
