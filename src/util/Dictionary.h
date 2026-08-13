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
struct DictLocation {
  std::string folderPath;  // dictionary base path (e.g. /dictionary/dict-en-en/dict-data)
  uint32_t offset = 0;     // byte offset in .dict file
  uint32_t size = 0;       // byte length in .dict file
  bool found = false;
};

class Dictionary {
 public:
  static constexpr unsigned long LONG_PRESS_MS = 600;

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

  // Search .idx for word (via .idx.oft if present). Returns file location without reading content.
  static DictLocation locate(const std::string& word, const DictLookupCallbacks& cbs = {},
                             const char* cachePath = nullptr);

  // Look up word in .idx (via .idx.oft if present). Returns definition or empty string.
  static std::string lookup(const std::string& word, const DictLookupCallbacks& cbs = {},
                            const char* cachePath = nullptr);

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

  // Read a null-terminated word from an open file into buf (max bufSize-1 chars).
  // Returns the number of characters read (excluding null), or -1 on error.
  static int readWordInto(HalFile& file, char* buf, size_t bufSize);

  // Read the word at ordinal `ordinal` in .idx.
  // folderPath is the dictionary base path (e.g. /dictionary/dict-en-en/dict-data).
  static std::string wordAtOrdinal(const std::string& folderPath, uint32_t ordinal);

  static std::string readDefinition(const std::string& folderPath, uint32_t offset, uint32_t size);

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

  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
