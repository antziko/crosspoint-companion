#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "util/Dictionary.h"

// Dictionary::LookupCtx exists so a probe sequence opens its files once instead of once per
// probe. Before it, every Dictionary::locate() call re-opened four files: dictionary.bin (via
// activeDictPath), .idx, the page index inside resolveScanBoundsIn, and the SAME page index
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

// A same-group fallback answer is scoped to the entry it produced, not to the reading
// session: everything reads activeDictPath() while that entry is on screen, but the next
// lookup starts from the configured dictionary again.
TEST_F(DictLookupSession, FallbackPromotionIsScopedToTheEntryItAnswered) {
  writeDictBin(kDictBase);
  EXPECT_EQ(Dictionary::activeDictPath(), kDictBase);

  Dictionary::promoteFallbackDictPath("/dictionaries/other/other");
  EXPECT_EQ(Dictionary::activeDictPath(), "/dictionaries/other/other");

  EXPECT_EQ(Dictionary::takeFallbackPromotion(), "/dictionaries/other/other");
  EXPECT_EQ(Dictionary::activeDictPath(), kDictBase);        // back to the configured one
  EXPECT_TRUE(Dictionary::takeFallbackPromotion().empty());  // nothing left to undo
}

// The long-press switch is the user's own choice: it outranks a promotion in force and
// cannot be undone by a later revert.
TEST_F(DictLookupSession, ExplicitSwitchOutranksAPromotion) {
  writeDictBin(kDictBase);
  Dictionary::promoteFallbackDictPath("/dictionaries/other/other");
  Dictionary::setSessionDictPath("/dictionaries/chosen/chosen");

  EXPECT_TRUE(Dictionary::takeFallbackPromotion().empty());
  EXPECT_EQ(Dictionary::activeDictPath(), "/dictionaries/chosen/chosen");
}

// A promotion taken over a user switch reverts to that switch, not to the configured
// dictionary — the revert target is whatever was in force, not always dictionary.bin.
TEST_F(DictLookupSession, PromotionRevertsToTheOverrideItReplaced) {
  writeDictBin(kDictBase);
  Dictionary::setSessionDictPath("/dictionaries/chosen/chosen");
  Dictionary::promoteFallbackDictPath("/dictionaries/other/other");

  EXPECT_EQ(Dictionary::takeFallbackPromotion(), "/dictionaries/other/other");
  EXPECT_EQ(Dictionary::activeDictPath(), "/dictionaries/chosen/chosen");
}

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
  // back to .oft. Losing that fallback is
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

// --- Buffered index scanning --------------------------------------------------
//
// The sequential scans read through a 512-byte window instead of one HalFile::read() per
// byte (each of which takes storageMutex, ~13 per ~19-byte entry). That is invisible in
// results, so what has to be pinned is that the window never desynchronises the scan: a
// headword or its 8-byte suffix straddling a refill must read back byte-identical, and the
// logical position() the loop bounds test must track the real file offset.

namespace {

// Append one .idx entry: headword, NUL, big-endian offset, big-endian size.
void appendIdxEntry(std::string& idx, const std::string& word, uint32_t offset, uint32_t size) {
  idx += word;
  idx += '\0';
  for (int shift : {24, 16, 8, 0}) idx += static_cast<char>((offset >> shift) & 0xFF);
  for (int shift : {24, 16, 8, 0}) idx += static_cast<char>((size >> shift) & 0xFF);
}

// A headword that sorts by its fixed-width numeric prefix, with a variable-length tail so
// successive entries land on every offset mod 512 and the straddles below are guaranteed
// rather than hoped for.
std::string syntheticWord(int i) {
  return "w" + std::string(4 - std::to_string(i).size(), '0') + std::to_string(i) +
         std::string(static_cast<size_t>(i % 7), 'x');
}

}  // namespace

TEST_F(DictLookupSession, ScanSurvivesEntriesStraddlingTheReadWindow) {
  constexpr size_t kWindow = 512;  // IdxScanner::BUF_SIZE
  constexpr int kEntries = 400;

  std::string idx;
  int wordsStraddling = 0;
  int suffixesStraddling = 0;
  for (int i = 0; i < kEntries; i++) {
    const std::string word = syntheticWord(i);
    const size_t wordStart = idx.size();
    const size_t suffixStart = wordStart + word.size() + 1;
    if (wordStart / kWindow != (wordStart + word.size() - 1) / kWindow) wordsStraddling++;
    if (suffixStart / kWindow != (suffixStart + 7) / kWindow) suffixesStraddling++;
    appendIdxEntry(idx, word, static_cast<uint32_t>(i * 100 + 1), static_cast<uint32_t>(i + 3));
  }
  // The point of the fixture: without these, the test would pass on a scanner that cannot
  // refill mid-entry at all.
  ASSERT_GT(wordsStraddling, 0);
  ASSERT_GT(suffixesStraddling, 0);
  ASSERT_GT(idx.size(), kWindow * 4);

  const fs::path dir = root_ / "dictionaries/synthetic";
  fs::create_directories(dir);
  std::ofstream(dir / "synthetic.idx", std::ios::binary).write(idx.data(), static_cast<std::streamsize>(idx.size()));
  writeDictBin("/dictionaries/synthetic/synthetic");

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  ASSERT_FALSE(ctx.hasPageIndex);  // no sidecar: every probe is a full sequential scan

  for (int i = 0; i < kEntries; i++) {
    const auto loc = Dictionary::locateIn(ctx, syntheticWord(i));
    ASSERT_TRUE(loc.found) << syntheticWord(i);
    EXPECT_EQ(loc.offset, static_cast<uint32_t>(i * 100 + 1)) << syntheticWord(i);
    EXPECT_EQ(loc.size, static_cast<uint32_t>(i + 3)) << syntheticWord(i);
  }
  EXPECT_FALSE(Dictionary::locateIn(ctx, "w0399zzz").found);
}

TEST_F(DictLookupSession, HeadwordLongerThanTheWordBufferStaysInSync) {
  // readWordInto truncates at 255 chars and drains the rest to keep the stream aligned with
  // the entry boundary. Spanning several refills, a drain that miscounts leaves every later
  // entry misread — so assert a word AFTER the overlong one still resolves.
  std::string idx;
  appendIdxEntry(idx, std::string(900, 'a'), 11, 22);  // sorts before both words below
  appendIdxEntry(idx, "beacon", 33, 44);
  appendIdxEntry(idx, "zenith", 55, 66);

  const fs::path dir = root_ / "dictionaries/overlong";
  fs::create_directories(dir);
  std::ofstream(dir / "overlong.idx", std::ios::binary).write(idx.data(), static_cast<std::streamsize>(idx.size()));
  writeDictBin("/dictionaries/overlong/overlong");

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));

  const auto beacon = Dictionary::locateIn(ctx, "beacon");
  ASSERT_TRUE(beacon.found);
  EXPECT_EQ(beacon.offset, 33u);
  EXPECT_EQ(beacon.size, 44u);

  const auto zenith = Dictionary::locateIn(ctx, "zenith");
  ASSERT_TRUE(zenith.found);
  EXPECT_EQ(zenith.offset, 55u);
  EXPECT_EQ(zenith.size, 66u);
}

TEST_F(DictLookupSession, AMissThatRunsToEndOfFileDoesNotPoisonTheNextProbe) {
  // A full-scan miss reads to EOF, which latches the scanner's end-of-file state. That state
  // is per-scan, and the widened retry's backward seek clears it — get either wrong and the
  // probe after a miss reads nothing.
  installDictionary("no-ifo", "/dictionaries/no-ifo", "/dictionaries/no-ifo/no-ifo");

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  ASSERT_FALSE(ctx.hasPageIndex);

  EXPECT_FALSE(Dictionary::locateIn(ctx, "zzzzzznotaword").found);  // scans to EOF
  EXPECT_TRUE(Dictionary::locateIn(ctx, "apple").found);            // must still work
  EXPECT_FALSE(Dictionary::locateIn(ctx, "zzzzzznotaword").found);
  EXPECT_TRUE(Dictionary::locateIn(ctx, "echo").found);
}

// --- Every entry of one headword (collectAll) ---------------------------------
//
// An index may list a headword several times (one entry per part of speech). Those entries are
// adjacent, but the page search lands on the last page whose first headword is <= the word, so
// a run that begins before a page boundary is entered at that page's first entry. collectAll
// must still return the whole run, first entry first.

namespace {

struct IdxEntry {
  std::string word;
  uint32_t offset;
  uint32_t size;
};

// Writes <dir>/<name>.idx and, when withOft, a .idx.oft whose pages hold 32 entries each.
void writeIndexedDictionary(const fs::path& dir, const std::string& name, const std::vector<IdxEntry>& entries,
                            const bool withOft) {
  fs::create_directories(dir);
  std::string idx;
  std::string oft(38, '\0');  // OFT_HEADER_SIZE; the reader does not validate its text
  for (size_t i = 0; i < entries.size(); i++) {
    if (i > 0 && i % 32 == 0) {
      const auto start = static_cast<uint32_t>(idx.size());
      for (int shift : {0, 8, 16, 24}) oft += static_cast<char>((start >> shift) & 0xFF);  // LE
    }
    appendIdxEntry(idx, entries[i].word, entries[i].offset, entries[i].size);
  }
  std::ofstream(dir / (name + ".idx"), std::ios::binary).write(idx.data(), static_cast<std::streamsize>(idx.size()));
  if (withOft) {
    std::ofstream(dir / (name + ".idx.oft"), std::ios::binary)
        .write(oft.data(), static_cast<std::streamsize>(oft.size()));
  }
}

// Filler headwords sorting before "ox" ("a000".."a099") and after it ("z000"..).
std::vector<IdxEntry> indexWithRun(const int fillerBefore, const int runLength, const int fillerAfter) {
  std::vector<IdxEntry> entries;
  for (int i = 0; i < fillerBefore; i++) {
    entries.push_back(
        {"a" + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i), static_cast<uint32_t>(i), 1});
  }
  for (int i = 0; i < runLength; i++) entries.push_back({"ox", static_cast<uint32_t>(1000 + i), 10});
  for (int i = 0; i < fillerAfter; i++) {
    entries.push_back(
        {"z" + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i), static_cast<uint32_t>(5000 + i), 1});
  }
  return entries;
}

}  // namespace

TEST_F(DictLookupSession, CollectAllReturnsARunWithinOnePage) {
  writeIndexedDictionary(root_ / "dictionaries/run", "run", indexWithRun(10, 3, 40), /*withOft=*/true);
  writeDictBin("/dictionaries/run/run");
  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  ASSERT_TRUE(ctx.hasPageIndex);

  const auto loc = Dictionary::locateIn(ctx, "ox", {}, /*collectAll=*/true);
  ASSERT_TRUE(loc.found);
  ASSERT_EQ(loc.entryCount(), 3);
  for (uint8_t i = 0; i < 3; i++) {
    EXPECT_EQ(loc.entry(i).offset, 1000u + i);
    EXPECT_STREQ(loc.label(i), "");
  }

  // Without collectAll the answer is the one it always was.
  const auto single = Dictionary::locateIn(ctx, "ox");
  ASSERT_TRUE(single.found);
  EXPECT_EQ(single.entryCount(), 1);
  EXPECT_EQ(single.offset, 1000u);
}

TEST_F(DictLookupSession, CollectAllFindsTheRunStartOnThePreviousPage) {
  // Entries 30..34 are "ox"; page 1 starts at entry 32, so the search lands on page 1 and the
  // run's first two entries sit on page 0.
  writeIndexedDictionary(root_ / "dictionaries/run", "run", indexWithRun(30, 5, 40), /*withOft=*/true);
  writeDictBin("/dictionaries/run/run");
  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));

  const auto loc = Dictionary::locateIn(ctx, "ox", {}, /*collectAll=*/true);
  ASSERT_TRUE(loc.found);
  ASSERT_EQ(loc.entryCount(), 5);
  for (uint8_t i = 0; i < 5; i++) EXPECT_EQ(loc.entry(i).offset, 1000u + i) << int(i);
}

// Writes a .idx .cspt the way Prepare builds one: a sample every 16 entries, entry 0 at byte 0,
// each a 16-byte null-padded prefix plus the LE .idx offset.
static void writeIdxCspt(const fs::path& path, const std::vector<IdxEntry>& entries) {
  std::string cspt = "CSPT";
  cspt += static_cast<char>(1);        // version
  cspt += static_cast<char>(16);       // prefix length
  cspt += std::string("\x10\x00", 2);  // stride 16, LE
  std::string samples;
  uint32_t count = 0;
  uint32_t pos = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    if (i % 16 == 0) {
      std::string prefix = entries[i].word.substr(0, 16);
      prefix.resize(16, '\0');
      samples += prefix;
      for (int shift : {0, 8, 16, 24}) samples += static_cast<char>((pos >> shift) & 0xFF);
      count++;
    }
    pos += static_cast<uint32_t>(entries[i].word.size() + 1 + 8);
  }
  for (int shift : {0, 8, 16, 24}) cspt += static_cast<char>((count >> shift) & 0xFF);
  cspt += samples;
  std::ofstream(path, std::ios::binary).write(cspt.data(), static_cast<std::streamsize>(cspt.size()));
}

TEST_F(DictLookupSession, CollectAllFindsThePreviousSampleThroughACspt) {
  // Same shape against a .cspt. Entries 14..18 are "ox",
  // so the search lands on the sample at entry 16 and entries 14 and 15 are behind it.
  const auto entries = indexWithRun(14, 5, 40);
  const fs::path dir = root_ / "dictionaries/run";
  writeIndexedDictionary(dir, "run", entries, /*withOft=*/true);
  writeIdxCspt(dir / "run.idx.oft.cspt", entries);
  writeDictBin("/dictionaries/run/run");

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  ASSERT_TRUE(ctx.pageIndexIsCspt);
  const auto loc = Dictionary::locateIn(ctx, "ox", {}, /*collectAll=*/true);
  ASSERT_EQ(loc.entryCount(), 5);
  for (uint8_t i = 0; i < 5; i++) EXPECT_EQ(loc.entry(i).offset, 1000u + i) << int(i);
  EXPECT_FALSE(ctx.hasOftFallback) << "the .cspt must have validated";
}

TEST_F(DictLookupSession, CollectAllWorksWithoutAPageIndex) {
  writeIndexedDictionary(root_ / "dictionaries/run", "run", indexWithRun(30, 4, 5), /*withOft=*/false);
  writeDictBin("/dictionaries/run/run");
  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  ASSERT_FALSE(ctx.hasPageIndex);

  const auto loc = Dictionary::locateIn(ctx, "ox", {}, /*collectAll=*/true);
  ASSERT_EQ(loc.entryCount(), 4);
  EXPECT_EQ(loc.entry(3).offset, 1003u);
}

TEST_F(DictLookupSession, CollectAllStopsAtTheCap) {
  writeIndexedDictionary(root_ / "dictionaries/run", "run", indexWithRun(10, 9, 10), /*withOft=*/true);
  writeDictBin("/dictionaries/run/run");
  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));

  const auto loc = Dictionary::locateIn(ctx, "ox", {}, /*collectAll=*/true);
  EXPECT_EQ(loc.entryCount(), 1 + DictLocation::MAX_EXTRA);
  EXPECT_EQ(loc.entry(0).offset, 1000u);
}

TEST_F(DictLookupSession, CollectAllWorksOnTheWidenedRetryHit) {
  // Out of cistrcmp order: page 1 opens with "pa", so the page search scans page 0 and misses,
  // and only the widened retry reaches the "ox" run behind it.
  std::vector<IdxEntry> entries = indexWithRun(32, 0, 0);
  entries.push_back({"pa", 900, 1});
  for (uint32_t i = 0; i < 3; i++) entries.push_back({"ox", 1000 + i, 10});
  for (uint32_t i = 0; i < 40; i++) entries.push_back({"z" + std::to_string(100 + i), 5000 + i, 1});
  writeIndexedDictionary(root_ / "dictionaries/run", "run", entries, /*withOft=*/true);
  writeDictBin("/dictionaries/run/run");
  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));

  const auto loc = Dictionary::locateIn(ctx, "ox", {}, /*collectAll=*/true);
  ASSERT_TRUE(loc.found);
  ASSERT_EQ(loc.entryCount(), 3);
  for (uint8_t i = 0; i < 3; i++) EXPECT_EQ(loc.entry(i).offset, 1000u + i) << int(i);
}

TEST(DictLocationEntries, RepeatedRangeIsOneEntry) {
  DictLocation loc;
  EXPECT_TRUE(loc.addEntry(10, 5));
  EXPECT_TRUE(loc.addEntry(10, 5));
  EXPECT_TRUE(loc.addEntry(10, 6));  // same offset, different range: a different definition
  EXPECT_EQ(loc.entryCount(), 2);
}

TEST(DictLocationEntries, LabelsStayWithTheirEntries) {
  DictLocation loc;
  loc.addEntry(1, 1, "\xE3\x82\x88\xE3\x82\x80");  // よむ
  loc.addEntry(2, 1);
  loc.addEntry(3, 1, "\xE3\x82\x88\xE3\x81\xB6");  // よぶ
  EXPECT_STREQ(loc.label(0), "\xE3\x82\x88\xE3\x82\x80");
  EXPECT_STREQ(loc.label(1), "");
  EXPECT_STREQ(loc.label(2), "\xE3\x82\x88\xE3\x81\xB6");
}

TEST(DictLocationEntries, ALabelThatDoesNotFitIsDroppedNotCut) {
  DictLocation loc;
  const std::string longLabel(70, 'x');
  loc.addEntry(1, 1, longLabel.c_str());
  loc.addEntry(2, 1, longLabel.c_str());  // 71 + 71 > 80: dropped
  loc.addEntry(3, 1, "ok");
  EXPECT_EQ(loc.entryCount(), 3);
  EXPECT_EQ(std::string(loc.label(0)), longLabel);
  EXPECT_STREQ(loc.label(1), "");
  EXPECT_STREQ(loc.label(2), "ok");
}

// --- Every .syn target of one form ---------------------------------------------

namespace {

// Appends one .syn entry: form, NUL, big-endian .idx ordinal.
void appendSynEntry(std::string& syn, const std::string& form, uint32_t ordinal) {
  syn += form;
  syn += '\0';
  for (int shift : {24, 16, 8, 0}) syn += static_cast<char>((ordinal >> shift) & 0xFF);
}

}  // namespace

TEST_F(DictLookupSession, AltFormsReturnsEveryHeadwordAFormPointsAt) {
  const fs::path dir = root_ / "dictionaries/syn";
  writeIndexedDictionary(dir, "syn", {{"yobu", 10, 1}, {"yomu", 20, 1}, {"zzz", 30, 1}}, /*withOft=*/false);
  std::string syn;
  appendSynEntry(syn, "yonda", 1);
  appendSynEntry(syn, "yonde", 1);
  appendSynEntry(syn, "yonde", 0);
  appendSynEntry(syn, "yonde", 1);  // repeated target
  appendSynEntry(syn, "yondo", 2);
  std::ofstream(dir / "syn.syn", std::ios::binary).write(syn.data(), static_cast<std::streamsize>(syn.size()));
  writeDictBin("/dictionaries/syn/syn");

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  std::vector<std::string> forms;
  Dictionary::resolveAltForms(ctx, "yonde", forms, 6);
  EXPECT_EQ(forms, (std::vector<std::string>{"yomu", "yobu"}));

  Dictionary::resolveAltForms(ctx, "yonda", forms, 6);
  EXPECT_EQ(forms, (std::vector<std::string>{"yomu"}));
  Dictionary::resolveAltForms(ctx, "absent", forms, 6);
  EXPECT_TRUE(forms.empty());
}

TEST_F(DictLookupSession, AltFormsFindsRunMembersOnThePreviousSynPage) {
  // 40 idx headwords; the .syn has filler forms then "yonde" at entries 30..33 with a page
  // boundary at entry 32, so the search lands on page 1 and two targets sit on page 0.
  const fs::path dir = root_ / "dictionaries/syn";
  std::vector<IdxEntry> idxEntries;
  for (int i = 0; i < 40; i++) {
    idxEntries.push_back(
        {"h" + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i), static_cast<uint32_t>(i), 1});
  }
  writeIndexedDictionary(dir, "syn", idxEntries, /*withOft=*/false);
  std::string syn;
  std::string oft(38, '\0');
  for (int i = 0; i < 40; i++) {
    if (i > 0 && i % 32 == 0) {
      const auto start = static_cast<uint32_t>(syn.size());
      for (int shift : {0, 8, 16, 24}) oft += static_cast<char>((start >> shift) & 0xFF);
    }
    if (i >= 30 && i < 34) {
      appendSynEntry(syn, "yonde", static_cast<uint32_t>(i - 30));
    } else {
      appendSynEntry(syn, (i < 30 ? "a" : "z") + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i),
                     static_cast<uint32_t>(i));
    }
  }
  std::ofstream(dir / "syn.syn", std::ios::binary).write(syn.data(), static_cast<std::streamsize>(syn.size()));
  std::ofstream(dir / "syn.syn.oft", std::ios::binary).write(oft.data(), static_cast<std::streamsize>(oft.size()));
  writeDictBin("/dictionaries/syn/syn");

  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
  std::vector<std::string> forms;
  Dictionary::resolveAltForms(ctx, "yonde", forms, 6);
  EXPECT_EQ(forms, (std::vector<std::string>{"h000", "h001", "h002", "h003"}));
}

namespace {

enum class IdxPageIndex { None, Oft, Cspt };

// 80 headwords "h000".."h079" and a .syn whose one form points at ordinals 70 and 33, past
// the first .oft page and the first .cspt sample.
void writeFarOrdinalDictionary(const fs::path& dir, const IdxPageIndex pageIndex) {
  std::vector<IdxEntry> entries;
  for (int i = 0; i < 80; i++) {
    entries.push_back(
        {"h" + std::string(3 - std::to_string(i).size(), '0') + std::to_string(i), static_cast<uint32_t>(i), 1});
  }
  writeIndexedDictionary(dir, "syn", entries, pageIndex == IdxPageIndex::Oft);
  if (pageIndex == IdxPageIndex::Cspt) writeIdxCspt(dir / "syn.idx.oft.cspt", entries);
  std::string syn;
  appendSynEntry(syn, "yonde", 70);
  appendSynEntry(syn, "yonde", 33);
  std::ofstream(dir / "syn.syn", std::ios::binary).write(syn.data(), static_cast<std::streamsize>(syn.size()));
}

}  // namespace

TEST_F(DictLookupSession, AltFormsResolveFarOrdinalsThroughEveryPageIndex) {
  for (const auto pageIndex : {IdxPageIndex::None, IdxPageIndex::Oft, IdxPageIndex::Cspt}) {
    fs::remove_all(root_ / "dictionaries/syn");
    writeFarOrdinalDictionary(root_ / "dictionaries/syn", pageIndex);
    writeDictBin("/dictionaries/syn/syn");
    Dictionary::LookupCtx ctx;
    ASSERT_TRUE(Dictionary::openLookupCtx(ctx));
    ASSERT_EQ(ctx.hasPageIndex, pageIndex != IdxPageIndex::None);
    ASSERT_EQ(ctx.pageIndexIsCspt, pageIndex == IdxPageIndex::Cspt);

    resetOpens();
    std::vector<std::string> forms;
    Dictionary::resolveAltForms(ctx, "yonde", forms, 6);
    EXPECT_EQ(forms, (std::vector<std::string>{"h070", "h033"})) << static_cast<int>(pageIndex);
    // Only the .syn itself: the headwords are read through the ctx's open .idx.
    EXPECT_EQ(opens(), 1) << static_cast<int>(pageIndex);
  }
}

TEST_F(DictLookupSession, AltFormsStopWhenCancelled) {
  writeFarOrdinalDictionary(root_ / "dictionaries/syn", IdxPageIndex::None);
  writeDictBin("/dictionaries/syn/syn");
  Dictionary::LookupCtx ctx;
  ASSERT_TRUE(Dictionary::openLookupCtx(ctx));

  DictLookupCallbacks cancelled;
  cancelled.shouldCancel = [](void*) { return true; };
  std::vector<std::string> forms;
  Dictionary::resolveAltForms(ctx, "yonde", forms, 6, cancelled);
  EXPECT_TRUE(forms.empty());
}
