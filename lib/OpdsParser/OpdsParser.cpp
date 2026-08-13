#include "OpdsParser.h"

#include <Logging.h>
#include <SdDebugLog.h>
#include <XmlParserUtils.h>
#include <esp_heap_caps.h>

#include <cstring>

namespace {
// Cap stored entries so a large unpaginated feed can't exhaust the heap and
// trigger abort() (bare `new` is not nothrow with -fno-exceptions). Parsing
// continues past the cap so the HTTP read completes cleanly; extra entries are
// dropped. We deliberately do NOT reserve() up front: a big contiguous reserve
// starves expat's own growing parse buffer mid-stream (it failed XML_GetBuffer
// on even the small root feed). 64 is low enough that the vector's doublings
// (…32→64) stay small transients, yet covers any usable page. Feeds that dump
// hundreds of entries unpaginated are a server problem (should send rel="next").
//
// The cap is the ONLY guard on free heap: an instantaneous free-heap check is
// useless here because the TLS stack holds its record buffers during the read, so
// the momentary free heap dips far below the before/after snapshots and any
// threshold misfires mid-stream (a small feed got cut to 4 entries that way).
constexpr size_t MAX_ENTRIES = 64;

// Entry-vector growth step. std::vector doubles by default, and a doubling is
// exactly the wrong shape here: at 32 entries it asks for one 6.4KB contiguous
// block on a heap the TLS read has already chopped into ~7KB pieces, so a feed
// truncated at 32 entries with 19KB still free (X3 log: "count=32 need=8448
// largest=7668" — short by 780 bytes). A fixed step keeps the request small as the
// feed grows, at the cost of one extra copy per step: an entry is ~100 bytes, so a
// full 64-entry page copies ~6KB across every step combined.
constexpr size_t OPDS_GROWTH_STEP = 8;

// Headroom (bytes) required beyond the entry vector's next-growth allocation before
// we let it reallocate. Covers the inserted entry's string copies plus safety. Below
// this, parsing stops adding entries (truncated) instead of aborting on a failed
// `new` — the X3's heap drops to ~2KB largest-block during the TLS read, so a large
// feed's vector growth would otherwise crash. See endElement.
constexpr size_t OPDS_GROWTH_HEAP_MARGIN = 2 * 1024;

// True if the largest contiguous free block can cover the entry vector's next
// growth step (from curCapacity) plus margin. When false, the caller stops adding
// entries rather than letting the reallocation's bare-`new` abort() on a starved
// heap. Logs the shortfall to SD for the (serial-less) X3.
bool heapCanGrowEntries(size_t curCapacity) {
  const size_t newCap = curCapacity + OPDS_GROWTH_STEP;
  const size_t needBytes = newCap * sizeof(OpdsEntry) + OPDS_GROWTH_HEAP_MARGIN;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (largest >= needBytes) return true;
  SdDebugLog::log("OPDS", "entries growth bailed: count=%u need=%u largest=%u free=%u", (unsigned)curCapacity,
                  (unsigned)needBytes, (unsigned)largest, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
  return false;
}
}  // namespace

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    errorDetail = "out of memory (parser alloc)";
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
    return;
  }
  // Pre-grow expat's parse buffer now, while the heap is still clean — before
  // the TLS read churns it with ~16KB mbedtls record buffers. expat needs ~4KB
  // contiguous (the buffer is bounded by XML_CONTEXT_BYTES + our 1KB feed chunk,
  // not by feed size — verified across 1KB..69KB feeds), and it never shrinks,
  // so reserving 8KB up front means it reuses that for the whole stream and
  // never needs a fragmentation-sensitive grow mid-read. That mid-stream grow
  // was failing XML_GetBuffer intermittently at ~73KB-free-but-fragmented,
  // producing "out of memory (parse buffer)" on larger letter feeds.
  if (!XML_GetBuffer(parser, 6144)) {
    errorOccured = true;
    errorDetail = "out of memory (parse buffer pregrow)";
    LOG_DBG("OPDS", "Couldn't pre-grow parse buffer");
    destroyXmlParser(parser);
    return;
  }
  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      errorDetail = "out of memory (parse buffer)";
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      errorLine = XML_GetCurrentLineNumber(parser);
      errorDetail = XML_ErrorString(XML_GetErrorCode(parser));  // static string, safe to keep
      LOG_DBG("OPDS", "Parse error at line %ld: %s", errorLine, errorDetail);
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  // write() already finalized-with-error and freed/nulled the parser, or the
  // ctor failed to allocate it. Bail so we don't run XML_Parse on a NULL parser,
  // which returns XML_ERROR_INVALID_ARGUMENT at line 0 and clobbers the real
  // error detail write() captured. (Stream destructor always calls flush().)
  if (errorOccured || !parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    errorLine = XML_GetCurrentLineNumber(parser);
    errorDetail = XML_ErrorString(XML_GetErrorCode(parser));
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = false;
  truncated = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = sHref;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        if (rel && type && strstr(rel, "opds-spec.org/acquisition") != nullptr &&
            strcmp(type, "application/epub+zip") == 0) {
          // Prefer plain EPUB links over derived formats when multiple
          // acquisition links are present for one entry.
          const bool isPlainEpub = strstr(href, ".epub") != nullptr || strstr(href, "/epub/") != nullptr;
          const bool alreadyHasPlainEpub = self->currentEntry.type == OpdsEntryType::BOOK &&
                                           (self->currentEntry.href.find(".epub") != std::string::npos ||
                                            self->currentEntry.href.find("/epub/") != std::string::npos);
          if (self->currentEntry.type != OpdsEntryType::BOOK || (isPlainEpub && !alreadyHasPlainEpub)) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            self->currentEntry.href = href;
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      // Drop entries past the cap. Parsing continues so the HTTP read finishes;
      // dropped => truncated flag.
      if (self->entries.size() >= MAX_ENTRIES) {
        self->truncated = true;
      } else if (self->entries.size() == self->entries.capacity() && !heapCanGrowEntries(self->entries.capacity())) {
        // Growing the vector reallocates (old + new block held at once) at the
        // moment the TLS read has the heap at its tightest. That bare-`new` aborts()
        // under -fno-exceptions on the X3 (a 37KB feed crashed here at ~7KB free).
        // Stop adding entries and mark the feed truncated instead of crashing. The
        // check is need-proportional, so small feeds (tiny growth steps) are
        // unaffected — only a large feed on a starved heap gets capped.
        self->truncated = true;
      } else {
        // Reserve the fixed step explicitly. Left to itself push_back would double,
        // which is the allocation heapCanGrowEntries() just sanctioned a step for —
        // and reserve() abort()s on failure under -fno-exceptions, so it must ask for
        // the size that was actually checked.
        if (self->entries.size() == self->entries.capacity()) {
          self->entries.reserve(self->entries.capacity() + OPDS_GROWTH_STEP);
        }
        self->entries.push_back(self->currentEntry);
      }
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inTitle || self->inAuthorName || self->inId) {
    self->currentText.append(s, len);
  }
}
