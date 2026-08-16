#pragma once
#include <Print.h>
#include <expat.h>

#include <string>
#include <vector>

#include "OpdsStringArena.h"

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 */
// Deliberately three strings, not four. An `id` field (the Atom <id>, typically a
// urn:uuid) was parsed and stored but never read anywhere in the tree. On the X3 that
// cost a live heap block per entry — ids exceed the 16-char SSO buffer, so a 40-entry
// feed pinned 40 extra small allocations across the browsing session — plus 40
// alloc/frees during the parse and 24 bytes per entry in the vector itself. The OPDS
// browser's failure mode is contiguous-block starvation, not free-heap exhaustion (a
// stuck session measured largest=9204 with 36KB still free), so scattered small
// allocations are exactly what hurts. If a future feature needs the id, re-add it and
// accept that cost knowingly.
//
// The three fields are bare `const char*` into an OpdsStringArena, not std::string. As
// strings this struct was 76 bytes, which made the entry vector's growth step the biggest
// single allocation of a parse and truncated real feeds: `entries growth bailed: count=48
// need=6304 largest=5876` is a 56 x 76 reallocation failing by 428 bytes with 12196 free.
// At 16 bytes the same step asks for 2944. The arena also collapses ~144 scattered string
// blocks per feed into 512-byte chunks. Pointers are NUL-terminated and stable for the
// arena's lifetime, so they are ordinary C strings.
//
// LIFETIME: an entry is only valid while the arena that owns its text is alive. Copying an
// OpdsEntry copies pointers, not text — see OpdsBookBrowserActivity::downloadBook, which
// takes owned std::strings before it releases the feed.
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  const char* title = "";
  const char* author = "";  // Only for books; "" when absent
  const char* href = "";    // Navigation URL or epub download URL
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   if (parser.parse(xmlData, xmlLength)) {
 *     for (const auto& entry : parser.getEntries()) {
 *       if (entry.type == OpdsEntryType::BOOK) {
 *         // Downloadable book
 *       } else {
 *         // Navigation link to another catalog
 *       }
 *     }
 *   }
 */
class OpdsParser final : public Print {
 public:
  OpdsParser();
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  bool error() const;

  operator bool() { return !error(); }

  // Expat error detail captured at the point of failure (valid after error()).
  // errorDetail is a static string from XML_ErrorString; errorLine is 0 if none.
  const char* getErrorDetail() const { return errorDetail; }
  long getErrorLine() const { return errorLine; }

  // True if entries were dropped by the memory guard (feed larger than RAM allows).
  bool wasTruncated() const { return truncated; }

  /**
   * Get the parsed entries (both navigation and book entries).
   * @return Vector of OpdsEntry entries
   */
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }

  /**
   * Hand the parsed feed to the caller: the entry vector is returned and the arena that
   * owns its text is moved into `arenaOut`. Both must be kept — an entry is a set of
   * pointers into that arena — which is why this replaces the old rvalue getEntries():
   * moving the vector alone would have left every string behind in the parser and every
   * pointer dangling the moment it was destroyed.
   */
  std::vector<OpdsEntry> takeEntries(OpdsStringArena& arenaOut);

  /** Chunks the arena is holding — for the SD trace, so feed cost stays visible. */
  size_t arenaChunks() const { return arena.chunkCount(); }
  size_t arenaBytes() const { return arena.bytesUsed(); }

  /**
   * Clear all parsed entries.
   */
  void clear();

 private:
  // Expat callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  // Helper to find attribute value
  static const char* findAttribute(const XML_Char** atts, const char* name);

  // Entry under construction. Deliberately std::string, not arena pointers: an entry is
  // only committed once it turns out to have both a title and an href, and text staged
  // straight into the arena for an entry that is then dropped could never be reclaimed
  // (the arena has no free-one). These three strings are reused for every entry in the
  // feed — assign() keeps their capacity — so they cost three allocations for the whole
  // parse, not three per entry.
  struct StagedEntry {
    OpdsEntryType type = OpdsEntryType::NAVIGATION;
    std::string title;
    std::string author;
    std::string href;
  };

  XML_Parser parser = nullptr;
  std::vector<OpdsEntry> entries;
  OpdsStringArena arena;
  StagedEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;

  bool errorOccured = false;
  const char* errorDetail = "";  // static string from XML_ErrorString
  long errorLine = 0;
  bool truncated = false;         // entries dropped by memory guard
  bool loggedGrowthBail = false;  // SD line for the growth shortfall written once per feed
};
