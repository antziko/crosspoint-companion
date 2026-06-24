#pragma once
#include <cstdint>
#include <string>

/**
 * Progress data from KOReader sync server.
 */
struct KOReaderProgress {
  std::string document;  // Document hash
  std::string progress;  // XPath-like progress string
  float percentage;      // Progress percentage (0.0 to 1.0)
  std::string device;    // Device name
  std::string deviceId;  // Device ID
  int64_t timestamp;     // Unix timestamp of last update
};

/**
 * One device's reading-stats entry for a document (self-hosted server extension).
 * The server stores one opaque JSON blob per device id; this is the parsed form.
 * Counters are per-device and monotonic — the merged total for display is the sum
 * across devices, never stored back into any single device's counter.
 */
struct KOReaderStatsEntry {
  char deviceId[24] = {0};        // Hash field name on the server
  uint32_t seconds = 0;           // "s": that device's lifetime reading seconds for the book
  uint32_t lastReadDayIndex = 0;  // "lr": days-since-2000 of last dated session (0 = none)
  uint8_t lastReadHour = 0;       // "lh"
  uint8_t lastReadMinute = 0;     // "lm"
};

/**
 * Optional fold callback for the cross-device dated reading-history merge.
 *
 * When passed to getStats(), it is invoked once per OTHER device whose stats
 * entry carries a dated-history section ("h"). `blob`/`len` are the
 * base64-DECODED bytes (a ReadingTimeHistory blob); the caller deserializes and
 * folds them into its own accumulator. Kept as a plain fn pointer + ctx (no
 * std::function — avoids heap/closure overhead) so this lib stays free of any
 * src/ ReadingTimeHistory dependency.
 */
struct StatsDatedFold {
  void* ctx = nullptr;
  void (*fn)(void* ctx, const uint8_t* blob, size_t len) = nullptr;
};

/**
 * HTTP client for KOReader sync API.
 *
 * Base URL: https://sync.koreader.rocks:443/
 *
 * API Endpoints:
 *   GET /users/auth - Authenticate (validate credentials)
 *   GET /syncs/progress/:document - Get progress for a document
 *   PUT /syncs/progress - Update progress for a document
 *   GET /syncs/bookmarks/:document - Get bookmarks for a document (self-hosted server extension)
 *   PUT /syncs/bookmarks - Update bookmarks for a document (self-hosted server extension)
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 */
class KOReaderSyncClient {
 public:
  enum Error { OK = 0, NO_CREDENTIALS, NETWORK_ERROR, AUTH_FAILED, SERVER_ERROR, JSON_ERROR, NOT_FOUND, LOW_MEMORY };

  /**
   * RAII guard for a keep-alive connection session.
   *
   * While an instance is alive, every sync request reuses ONE keep-alive TLS
   * connection (a single handshake) instead of opening a fresh connection per
   * leg. This avoids the ESP32-C3 LWIP TIME_WAIT / socket churn that makes rapid
   * back-to-back reconnects (e.g. a PUT immediately after a GET) fail with
   * sock<0 and ~16s connect timeouts. Construct one around a full sync sequence;
   * the underlying connection is closed when the guard is destroyed.
   */
  class SyncSession {
   public:
    SyncSession();
    ~SyncSession();
    SyncSession(const SyncSession&) = delete;
    SyncSession& operator=(const SyncSession&) = delete;
  };

  /**
   * Authenticate with the sync server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Get reading progress for a document.
   * @param documentHash The document hash (from KOReaderDocumentId)
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress);

  /**
   * Get the bookmarks blob for a document (self-hosted server extension).
   *
   * The server stores bookmarks as an opaque pre-serialized JSON-array string;
   * this returns that string verbatim for the caller to parse.
   *
   * @param documentHash The document hash (must match the progress hash for the book)
   * @param outBookmarksJson Output: the JSON-array string (empty if server stored none)
   * @return OK on success, NOT_FOUND if no bookmarks exist, error code on failure
   */
  static Error getBookmarks(const std::string& documentHash, std::string& outBookmarksJson);

  /**
   * Replace the bookmarks blob for a document (self-hosted server extension).
   * @param documentHash The document hash
   * @param bookmarksJson Pre-serialized JSON-array string of all bookmarks
   * @return OK on success, error code on failure
   */
  static Error updateBookmarks(const std::string& documentHash, const std::string& bookmarksJson);

  /** Max device entries parsed from a stats response; extras are dropped. */
  static constexpr size_t MAX_STATS_DEVICES = 8;

  /**
   * Get per-device reading-stats entries for a document (self-hosted server extension).
   * @param documentHash The document hash (must match the progress hash for the book)
   * @param outEntries Caller-provided array of MAX_STATS_DEVICES entries
   * @param outCount Output: number of entries filled
   * @param fold Optional dated-history fold callback. When non-null, each OTHER
   *   device's base64-decoded "h" section is passed to fold->fn for the caller to
   *   merge (cross-device dated history). Self entry and entries without "h" are
   *   skipped. Used only for the global pseudo-document.
   * @param dictFold Optional dictionary-history fold callback. Same contract as
   *   `fold` but for each OTHER device's base64-decoded "dh" section (per-book
   *   lookup-history merge). Self entry and entries without "dh" are skipped.
   * @param fcFold Optional flashcard fold callback. Same contract as `fold` but
   *   for each OTHER device's base64-decoded "fc" section (per-book flashcard
   *   deck merge). Self entry and entries without "fc" are skipped.
   * @return OK on success, NOT_FOUND if no stats exist, error code on failure
   */
  static Error getStats(const std::string& documentHash, KOReaderStatsEntry* outEntries, size_t& outCount,
                        const StatsDatedFold* fold = nullptr, const StatsDatedFold* dictFold = nullptr,
                        const StatsDatedFold* fcFold = nullptr);

  /**
   * Replace THIS device's stats blob for a document (self-hosted server extension).
   * Other devices' blobs are untouched (one hash field per device on the server).
   * @param documentHash The document hash
   * @param entry The local device's counters (deviceId field is ignored; deviceId() is sent)
   * @param dated Optional pre-serialized ReadingTimeHistory blob bytes; when non-null
   *   they are base64-encoded into an "h" field appended to the stats blob (global
   *   pseudo-document only — per-book PUTs pass nullptr and keep the small blob).
   * @param datedLen Length of `dated` in bytes (ignored when dated is null).
   * @param dict Optional per-book dictionary-history blob (LookupHistory::serializeBlob);
   *   when non-null it is base64-encoded into a "dh" field appended to the stats blob.
   * @param dictLen Length of `dict` in bytes (ignored when dict is null).
   * @param fc Optional per-book flashcard blob (FlashcardDeck::serializeForUpload);
   *   when non-null it is base64-encoded into an "fc" field appended to the stats blob.
   * @param fcLen Length of `fc` in bytes (ignored when fc is null).
   * @return OK on success, error code on failure
   */
  static Error updateStats(const std::string& documentHash, const KOReaderStatsEntry& entry,
                           const uint8_t* dated = nullptr, size_t datedLen = 0, const uint8_t* dict = nullptr,
                           size_t dictLen = 0, const uint8_t* fc = nullptr, size_t fcLen = 0);

  /**
   * Unique, stable per-chip device id ("crosspoint-<efuse mac hex>") sent as
   * device_id in progress and stats uploads.
   */
  static const char* deviceId();

  /**
   * Server capability/version tag (e.g. "stats-v1") echoed by the stats-enabled
   * self-hosted server in its updateStats response. Empty string when the last
   * updateStats got no tag — i.e. a server without the stats extension.
   * Valid after the most recent updateStats() call.
   */
  static const char* statsServerTag();

  /**
   * Get human-readable error message.
   */
  static const char* errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /**
   * Cumulative transfer counters for the sync summary. resetByteCounters() zeroes
   * both at the start of a sync; bytesDown() sums GET response-body bytes, bytesUp()
   * sums PUT request-body bytes, across every leg run since the reset.
   */
  static void resetByteCounters();
  static uint32_t bytesDown();
  static uint32_t bytesUp();
};
