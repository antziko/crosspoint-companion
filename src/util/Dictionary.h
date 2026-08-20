#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Helper for constructing dictionary file paths from a folder base path.
struct DictPaths {
  const std::string& folder;
  explicit DictPaths(const std::string& f) : folder(f) {}

  std::string idx() const { return folder + ".idx"; }
  std::string dict() const { return folder + ".dict"; }
  std::string syn() const { return folder + ".syn"; }
  std::string ifo() const { return folder + ".ifo"; }
  std::string idxOft() const { return folder + ".idx.oft"; }
  std::string synOft() const { return folder + ".syn.oft"; }
  std::string idxOftCspt() const { return folder + ".idx.oft.cspt"; }
  std::string synOftCspt() const { return folder + ".syn.oft.cspt"; }
  std::string dictDz() const { return folder + ".dict.dz"; }
  std::string synDz() const { return folder + ".syn.dz"; }
};

// Plain-function-pointer callbacks for Dictionary::lookup.
// Zero overhead: no heap allocation, no vtable, no std::function bloat.
struct DictLookupCallbacks {
  void* ctx = nullptr;
  void (*onProgress)(void* ctx, int percent) = nullptr;
  bool (*shouldCancel)(void* ctx) = nullptr;
};

// Metadata parsed from a StarDict .ifo file.
struct DictInfo {
  char bookname[128] = "";
  char website[128] = "";
  char date[32] = "";
  char description[256] = "";
  char sametypesequence[16] = "";
  uint32_t wordcount = 0;
  uint32_t altFormCount = 0;
  uint32_t idxfilesize = 0;
  bool hasAltForms = false;
  bool isCompressed = false;  // .dict.dz present but no .dict
  char lang[32] = "";         // e.g. "en-en", "el-el"
  bool valid = false;
};

// Result of an index search — file location of a definition without reading it.
// Why a lookup returned nothing, so the UI can tell "this word is not in the dictionary"
// from "there is no usable dictionary to look it up in". Both used to surface as
// "Not found", which sent the user hunting for a spelling mistake when the real problem
// was an unset or unreadable dictionary.
//
// Deliberately NOT modelled on upstream's LookupResult: its LowMemory and Decompress
// states describe a .dict.dz inflate that cannot happen here — this engine extracts
// during DictPrepareActivity, so the lookup path performs no large allocation.
enum class LookupStatus : uint8_t {
  Found,
  NotFound,      // the search ran and the word is genuinely absent
  NoDictionary,  // no dictionary configured (no dictionary.bin, or it is empty)
  ReadError,     // a dictionary is configured but its .idx could not be opened
};

struct DictLocation {
  std::string folderPath;  // dictionary base path (e.g. /dictionary/dict-en-en/dict-data)
  uint32_t offset = 0;     // byte offset in .dict file
  uint32_t size = 0;       // byte length in .dict file
  bool found = false;
  LookupStatus status = LookupStatus::NotFound;
};

class Dictionary {
 public:
  // 400, matching ReaderUtils::BOOKMARK_HOLD_MS — the two hold-Confirm gestures are mutually
  // exclusive by setting, so there is no reason for them to feel different. Was 600: pure dead
  // time with no on-screen feedback (an indicator would cost a 435ms panel repaint, i.e. more
  // than the wait it disguises), and it fronted a press-to-highlight path already ~1.7s long.
  static constexpr unsigned long LONG_PRESS_MS = 400;

  // Returns the active dictionary folder base path by reading dictionary.bin from the SD card.
  // If cachePath is non-null and non-empty, reads <cachePath>/dictionary.bin (per-book override).
  // Otherwise reads /.crosspoint/dictionary.bin (global setting).
  // Returns empty string if no dictionary is configured or the file cannot be read.
  static std::string readDictPath(const char* cachePath = nullptr);

  // Writes folderPath to /.crosspoint/dictionary.bin (global setting).
  // Pass empty string to clear the global dictionary.
  static void saveGlobalDictPath(const char* folderPath);

  // --- Session override -----------------------------------------------------
  // A dictionary chosen for the current reading session only. Nothing is written
  // to the SD card, and the configured per-book / global selection is untouched:
  // readDictPath() keeps reporting what dictionary.bin says, so the settings
  // picker and the reader menu still show (and save) the real selection.
  //
  // THREADING: the lookup itself runs on DictLookupTask, a separate FreeRTOS task.
  // setSessionDictPath must only be called from the UI task while no lookup is in
  // flight (DictionaryLookupController::isActive() == false). The xTaskCreate inside
  // startLookup() is then the barrier that publishes the new path to the lookup task.
  // That invariant is what lets this be a plain char[] instead of a mutex — do not
  // call the setter from anywhere that can race a running lookup.
  static void setSessionDictPath(const char* folderPath);

  // Install `folderPath` for the entry a same-group fallback sweep just answered. As
  // visible as setSessionDictPath — everything resolves through activeDictPath(), so the
  // header name, the long-press cycle origin and the dictionary a flashcard records all
  // name the dictionary whose entry is actually on screen — but marked TRANSIENT: it is
  // scoped to that entry, not to the reading session. takeFallbackPromotion() undoes it so
  // the next lookup starts from the dictionary the user configured, and an explicit
  // setSessionDictPath (the user's own long-press choice) outranks and clears the mark.
  // Same threading rule as setSessionDictPath.
  static void promoteFallbackDictPath(const char* folderPath);

  // Undo a fallback promotion, restoring whatever was in force before it, and return the
  // path it had installed ("" when no promotion is in force — including after an explicit
  // setSessionDictPath). Callers re-promote it if the lookup they cleared it for fails and
  // the promoted entry stays on screen. Same threading rule as setSessionDictPath.
  static std::string takeFallbackPromotion();

  // True when the path currently in force was installed by promoteFallbackDictPath and
  // not superseded since. Lets a caller that saves and restores the session path put it
  // back with the transiency it had, instead of promoting it to an explicit choice.
  static bool sessionPathIsFallbackPromotion();

  // The session override if one is set, otherwise the configured path for cachePath.
  // This is what every lookup resolves through; readDictPath() is the configured value.
  static std::string activeDictPath(const char* cachePath = nullptr);

  // RAII clear for the session override. Held as a member by every activity that can
  // host the lookup flow, so the override cannot outlive the screen that set it —
  // activities are heap-allocated and deleted on exit (main.cpp:132-143), so the
  // destructor is guaranteed to run. Preferred over a bare setSessionDictPath("") in
  // each onExit(), which a future host would eventually forget.
  struct SessionOverrideScope {
    SessionOverrideScope() = default;
    ~SessionOverrideScope() { setSessionDictPath(""); }
    SessionOverrideScope(const SessionOverrideScope&) = delete;
    SessionOverrideScope& operator=(const SessionOverrideScope&) = delete;
  };

  // Returns true if a dictionary is configured and all required files exist.
  static bool exists(const char* cachePath = nullptr);

  // Returns true if a .syn file exists for the active dictionary.
  // Gates all alternate-form UI — checked at runtime against the physical file.
  static bool hasAltForms(const char* cachePath = nullptr);

  // Validates the dictionary path stored in /.crosspoint/dictionary.bin against the SD card.
  // If the path is missing or the required files are gone, clears the file. Returns true if valid.
  static bool isValidDictionary();

  // Parse the .ifo file in folderPath into `info`, returning info.valid. Resets `info`
  // first, so one scratch object can be reused across several dictionaries.
  // Also checks for .syn and .dict.dz presence.
  //
  // Prefer this over readInfo() wherever the result would land on the stack: DictInfo is
  // 608 bytes, well past the 256-byte budget CLAUDE.md sets for locals.
  static bool readInfoInto(const char* folderPath, DictInfo& info);

  // By-value form, for callers that want a throwaway. Note the 608 bytes land in the
  // caller's frame — see readInfoInto above.
  static DictInfo readInfo(const char* folderPath);

  // The .idx + page-index handles shared by every locate() in one lookup, so a probe
  // sequence opens them once instead of once per probe.
  //
  // A miss costs four SD opens per locate(): dictionary.bin (via activeDictPath), .idx,
  // the page index inside resolveScanBounds(), and the SAME page index again inside the
  // widened-retry path. The stem-variant fallback runs up to six probes, so a missed word
  // was costing ~28 opens and ~42 transient std::strings (every DictPaths accessor returns
  // by value) before findSimilar() even started — heap churn on precisely the path whose
  // fragmentation makes lookups fail mid-session.
  //
  // Handles stay open for the ctx's lifetime; every reader seeks before it reads, so they
  // are safe to share across probes. All fixed-size members: no heap, nothing to free
  // (DESTRUCTOR_CLOSES_FILE=1 closes both files at scope exit).
  //
  // NOT thread-safe and must NOT be shared across tasks: runLookup() probes the exact word
  // on DictLookupTask while the stem loop runs on the UI task. Each opens its own ctx.
  struct LookupCtx {
    HalFile idx;
    HalFile pageIndex;  // .idx.oft.cspt when present, else .idx.oft
    // Opened lazily, and only if a .cspt search fails (stale/malformed sidecar), to preserve
    // the .cspt -> .oft fallback the path-based resolveScanBounds has. Without it a bad
    // sidecar would demote every probe to a full-file scan.
    HalFile oftFallback;
    bool pageIndexIsCspt = false;
    bool hasPageIndex = false;
    bool oftTried = false;
    bool hasOftFallback = false;
    uint32_t idxSize = 0;
    char base[128] = "";  // resolved dictionary base path, "" when none is configured
    bool valid = false;
  };

  // Resolve the active dictionary once and open .idx (required) plus the page index
  // (optional — locate falls back to a full scan without it). False when no dictionary is
  // configured or .idx will not open; ctx.base is still filled when only .idx failed, so
  // callers can tell "no dictionary" from "unreadable dictionary".
  static bool openLookupCtx(LookupCtx& ctx, const char* cachePath = nullptr);

  // openLookupCtx against an explicit dictionary base path, skipping activeDictPath(). Same
  // semantics otherwise, including the full ctx reset — the same-category fallback sweep reuses
  // one ctx across several dictionaries and relies on that reset to drop the previous one's
  // handles and lazily-opened .oft before the next probe.
  static bool openLookupCtxAt(LookupCtx& ctx, const char* basePath);

  // locate() against an already-open ctx. Identical semantics to locate(); this is the form
  // to use when probing several candidate spellings for one word.
  static DictLocation locateIn(LookupCtx& ctx, const std::string& word, const DictLookupCallbacks& cbs = {});

  // Search .idx for word (via .idx.oft if present). Returns file location without reading content.
  // Thin wrapper: opens a LookupCtx and calls locateIn(). Prefer the ctx form for probe loops.
  static DictLocation locate(const std::string& word, const DictLookupCallbacks& cbs = {},
                             const char* cachePath = nullptr);

  // NOTE: there is deliberately no lookup()-returns-the-definition entry point. Definitions
  // are streamed from .dict in 512-byte chunks (DictHtmlRenderer::renderFromFileStreaming),
  // never materialised in RAM — a definition can be tens of KB and this heap cannot take it.
  // Callers locate() first, then stream from the returned offset/size.

  // Look up word in .syn (via .syn.oft if present).
  // Returns the canonical headword from .idx, or empty string if not found.
  static std::string resolveAltForm(const std::string& word, const char* cachePath = nullptr);

  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Returns up to maxResults words from .idx that are close in edit distance to word.
  // Requires .idx to be accessible; uses .idx.oft if present for neighbourhood search.
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults, const char* cachePath = nullptr);

  // Reads .idx.oft.cspt header and returns entryCount.
  // Returns 0 if the file is missing, too small, or has invalid magic/version.
  // Cheap: one SD seek, 12 bytes read.
  static uint32_t readCsptEntryCount(const char* cachePath = nullptr);

 private:
  // Shared word read buffer. Lookup functions are single-threaded; this avoids
  // putting a 256-byte array on the stack in every caller (and 512B peak when nested).
  static char wordBuf[256];

  // Session override path; empty means "no override". Fixed array rather than a
  // std::string so it costs no heap and no global constructor. 128 matches the
  // char binPath[128] that readDictPath already assumes for dictionary paths.
  static char sessionPath[128];
  // What sessionPath held before the fallback promotion in force, and whether one is: the
  // revert target for takeFallbackPromotion(). Static buffers rather than std::string for
  // the same reason sessionPath is one — this is touched on the lookup path, where the
  // largest free block can be a few KB.
  static char preFallbackPath[128];
  static bool sessionPathIsFallback;

  // Read a null-terminated word from an open file into buf (max bufSize-1 chars).
  // Returns the number of characters read (excluding null), or -1 on error.
  static int readWordInto(HalFile& file, char* buf, size_t bufSize);

  // Read-ahead window over one .idx, for the SEQUENTIAL scans only.
  //
  // Every HalFile call takes storageMutex (HalStorage.cpp:172), and the raw scans spend one
  // on each of: the loop's position() test, every byte of the headword, and the 8-byte
  // suffix — ~13 per ~19-byte entry, which is why a scan measures ~115KB/s on device. Serving
  // those from a block window makes it one per BUF_SIZE bytes, and position() free.
  //
  // Deliberately NOT used by findPageBounds: that is a binary search of random seeks, where a
  // read-ahead window is filled and discarded on every probe.
  //
  // The buffer is static for the same reason wordBuf is — the lookup path is single-threaded
  // (runLookup joins its task before the UI task probes again) and 512 bytes is twice the
  // whole per-function stack budget.
  class IdxScanner {
   public:
    IdxScanner(HalFile& file, uint32_t startPos) : file_(file), pos_(startPos) { file_.seekSet(startPos); }

    // Logical offset of the next byte, answered without touching the file.
    uint32_t position() const { return pos_; }

    // Next byte, or -1 at EOF / on error.
    int readByte();
    // Exactly `count` bytes; false if fewer were available.
    bool readBytes(void* dst, size_t count);
    // Re-point the window. Only the widened retry needs this.
    void seek(uint32_t offset);

   private:
    // Refill the window from the file. False when nothing more could be read.
    bool refill();

    static constexpr size_t BUF_SIZE = 512;
    static uint8_t buf_[BUF_SIZE];

    HalFile& file_;
    uint32_t pos_ = 0;   // logical file offset of the next byte to return
    size_t avail_ = 0;   // valid bytes currently in buf_
    size_t cursor_ = 0;  // next unread index into buf_
    bool eof_ = false;
  };

  // readWordInto against a scanner window. Same contract as the HalFile form.
  static int readWordInto(IdxScanner& scanner, char* buf, size_t bufSize);

  // Build "<base><suffix>" into a caller-supplied buffer. The hot lookup path uses this
  // instead of the DictPaths accessors, which return std::string by value — ~60-char paths
  // are well past SSO, so each accessor call is a heap round trip, and with -fno-exceptions
  // a failed one abort()s rather than returning null. Mirrors buildDictPath() in
  // DictionaryDefinitionActivity.cpp, which exists for the same reason. DictPaths stays for
  // cold callers (settings and registry screens), where the churn does not matter.
  // Returns false when the result would be truncated.
  static bool buildPath(char* buf, size_t bufSize, const char* base, const char* suffix);

  // Read the word at ordinal `ordinal` in .idx.
  // folderPath is the dictionary base path (e.g. /dictionary/dict-en-en/dict-data).
  static std::string wordAtOrdinal(const std::string& folderPath, uint32_t ordinal);

  // Binary search .oft to find the page boundary bytes in src containing target.
  // On return, *startByte and *endByte delimit the 32-word page to scan linearly.
  // srcFileSize is used as the upper bound when the page is the last one.
  static void findPageBounds(HalFile& oft, HalFile& src, uint32_t srcFileSize, const char* target, uint32_t* startByte,
                             uint32_t* endByte);

  // Binary search .idx.oft.cspt to find the scan range in .idx containing target.
  // Returns true if .cspt was valid and bounds were set, false to fall back to .oft.
  static bool binarySearchCspt(HalFile& cspt, const char* target, uint32_t idxFileSize, uint32_t* startByte,
                               uint32_t* endByte);

  // Resolve the byte range in src to scan for target. Tries .cspt first; on miss or
  // absence, falls back to .oft. Callers must initialize *startByte=0, *endByte=srcFileSize
  // before calling — when both .cspt and .oft are absent, the bounds are left untouched
  // (full-file scan). Used by both .idx (locate) and .syn (resolveAltForm) lookups.
  static void resolveScanBounds(const char* csptPath, const char* oftPath, HalFile& src, uint32_t srcFileSize,
                                const char* target, uint32_t* startByte, uint32_t* endByte);

  // resolveScanBounds against an already-open ctx. Same .cspt-then-.oft preference order as
  // the path-based form; takes the whole ctx because the .oft fallback is opened lazily.
  static void resolveScanBoundsIn(LookupCtx& ctx, const char* target, uint32_t* startByte, uint32_t* endByte);

  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
