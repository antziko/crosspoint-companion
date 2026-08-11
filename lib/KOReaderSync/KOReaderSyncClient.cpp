#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <SecureHttpClient.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <ctime>
#include <memory>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

// Cumulative GET/PUT byte counters for the sync summary. File-scope so every leg
// (member function) below can add to them; reset once per sync via resetByteCounters().
static uint32_t s_bytesDown = 0;
static uint32_t s_bytesUp = 0;

void KOReaderSyncClient::resetByteCounters() {
  s_bytesDown = 0;
  s_bytesUp = 0;
}
uint32_t KOReaderSyncClient::bytesDown() { return s_bytesDown; }
uint32_t KOReaderSyncClient::bytesUp() { return s_bytesUp; }

namespace {
// Server capability tag from the last updateStats response (see statsServerTag()).
char statsServerTagBuf[32] = {0};

// Upper bound on a decoded dated-history blob (ReadingTimeHistory::BLOB_MAX_BYTES
// is 876; round up for headroom). Bounds the single reusable decode buffer in the
// streaming fold — a malformed/oversized "h" is rejected rather than allocated for.
constexpr size_t kStatsDatedMaxBytes = 1024;

// Upper bound on a decoded per-book dictionary-history blob ("dh"). Matches the
// 4 KB serializeBlob cap on the sender; bounds the reusable decode buffer.
constexpr size_t kStatsDictMaxBytes = 4096;

// Upper bound on a decoded per-book flashcard blob ("fc"). Matches the sender's
// FlashcardDeck::FC_SLICE_CEIL adaptive cap; bounds the reusable decode buffer.
constexpr size_t kStatsFcMaxBytes = 6144;

// Hard ceiling on a single HTTP response body. The stats GET aggregates every
// device's blob (each may carry a base64 "dh" up to ~5.5 KB), so the body scales
// with device count; cap the unbounded realloc so a pathological response fails
// clean instead of exhausting the heap. 64 KB covers the realistic device range.
constexpr int kMaxResponseBytes = 64 * 1024;
// Contiguous-heap headroom kept free while the response string grows. The append
// happens with the TLS connection live (wolfSSL record buffers resident), so leave
// room rather than consuming the very last block.
//
// Two sites take a response buffer, and they need different guards because they cost
// different amounts of contiguous heap.
//
// reserve-once (Content-Length known): r.body is still empty, so this is a single fresh
// allocation with no old block held alongside it. Only kReserveHeadroom is needed, to avoid
// claiming the last usable block.
constexpr size_t kReserveHeadroom = 1024;
//
// growth (chunked / unknown length): libstdc++ allocates the new capacity while the old
// buffer is still live and copies between them, so the transient peak really is roughly twice
// the target — hence a margin scaled to the buffer being taken. A flat 8 KB was worse in the
// other direction: it made every response demand 8 KB no matter how small, and device logs
// show STATS_PUT (a 90-byte response) and BOOKMARKS_PUT (70 bytes) aborting as "low-heap"
// three times running at largest=10740, on a heap that could hold them hundreds of times over.
// The cap keeps large responses at exactly the old 8 KB behaviour.
constexpr size_t kResponseMarginMin = 1024;
constexpr size_t kResponseMarginMax = 8 * 1024;
constexpr size_t responseHeapMargin(const size_t want) {
  if (want < kResponseMarginMin) return kResponseMarginMin;
  if (want > kResponseMarginMax) return kResponseMarginMax;
  return want;
}
}  // namespace

const char* KOReaderSyncClient::deviceId() {
  // Unique, stable per-chip id ("crosspoint-a1b2c3d4e5f6") from the factory eFuse
  // MAC. Required for per-device stats merging on the sync server: X3 and X4 must
  // not share an id (the old "crosspoint-reader" constant made every CrossPoint
  // device indistinguishable). Formatted once into a static buffer.
  static char id[24] = {0};
  if (id[0] == '\0') {
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(id, sizeof(id), "crosspoint-%012llx", static_cast<unsigned long long>(mac));
  }
  return id;
}

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";

// Hold WiFi out of modem-sleep for the duration of a request, then restore the
// default. At WIFI_PS_MIN_MODEM the radio sleeps between DTIM beacons; on a
// marginal link that drops handshake packets and the TLS handshake stalls to a
// timeout — observed on the X3, where even the first KOSync GET never connected
// while the X4 (more headroom) merely ran slow. HttpDownloader already does this
// for OPDS/downloads. RAII so every return path restores power-save.
struct NoWifiSleep {
  NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

// --- Connection reuse (keep-alive) session ---
// One reused SecureHttpClient across every leg of a sync, so a sync pays a single
// wolfSSL handshake instead of one per leg — and avoids the ESP32-C3 rapid-
// reconnect churn (LWIP TIME_WAIT / sock<0 / ~16s connect timeouts) that made a
// PUT immediately after a GET fail. Non-null only between beginSession() and
// endSession(); one-shot requests (no session) use a local client that sends
// Connection: close and releases the socket cleanly after the request.
//
// wolfSSL (SecureNet) replaces the old mbedTLS/esp_http_client path: a handshake
// to the Cloudflare 3-cert chain now fits in ~35-43 KB of *small* allocations
// (SP-ECC, FP_MAX_BITS=8192) instead of needing ~55 KB free + two ~16 KB
// contiguous record slabs. That dissolves the heap-gate machinery this file used
// to carry (MIN_HEAP_FOR_TLS / contigOkForPut / failed-alloc probing): both a
// cold handshake and back-to-back GET+PUT fit the post-WiFi heap now.
std::unique_ptr<freeink::SecureHttpClient> s_sessionClient;
bool s_sessionActive = false;

void beginSession() { s_sessionActive = true; }  // client lazily created on first request
void endSession() {
  s_sessionClient.reset();  // closes the kept-alive connection
  s_sessionActive = false;
}

// SD trace: pre-request heap+RSSI snapshot under `tag` (mirrors HttpDownloader's
// per-request trace). Returns the request start time for the paired resp line.
uint32_t koTraceReq(const char* tag, size_t bodyLen) {
  const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
  SdDebugLog::log("KOSYNC", "%s req body=%u heap=%u largest8=%u intFree=%u rssi=%d", tag, (unsigned)bodyLen,
                  snap.heapFree, snap.largest8Bit, snap.internalFree, (int)snap.rssi);
  return millis();
}
void koTraceResp(const char* tag, int status, size_t bytes, uint32_t startMs) {
  const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
  SdDebugLog::log("KOSYNC", "%s resp code=%d elapsed=%lums bytes=%u heap=%u largest8=%u", tag, status,
                  (unsigned long)(millis() - startMs), (unsigned)bytes, snap.heapFree, snap.largest8Bit);
}

// Outcome of a KOSync request: HTTP status + response body.
struct KoResponse {
  int status = 0;            // HTTP status code; <=0 => transport failure (no response)
  bool transportOk = false;  // true once a status line was read
  std::string body;          // response body (capped at kMaxResponseBytes)
  // Set when the transfer was abandoned by the heap guards below rather than by the network.
  // Callers that retry need the distinction: a transient connect failure is worth another
  // attempt, an out-of-contiguous-heap abort is not — nothing changes between attempts, so
  // the retry re-runs a full TLS handshake to fail on the identical block.
  bool heapAbort = false;
};

// Perform one KOSync request over wolfSSL (SecureHttpClient). `method` is
// "GET"/"PUT"/"POST"; `body` is null for GET. Attaches the KOSync auth headers +
// pinned-root CA, streams the response into a 64 KB-capped buffer, updates the
// byte counters + lastHttpCode, and reuses the session connection when one is
// active (otherwise a one-shot local client that closes after).
KoResponse koPerform(const char* method, const std::string& url, const std::string* body, const char* tag) {
  KoResponse r;
  const NoWifiSleep noWifiSleep;
  const uint32_t startMs = koTraceReq(tag, body ? body->size() : 0);

  freeink::SecureHttpClient local;
  freeink::SecureHttpClient* http = &local;
  if (s_sessionActive) {
    if (!s_sessionClient) {
      s_sessionClient = makeUniqueNoThrow<freeink::SecureHttpClient>();
      if (!s_sessionClient) {
        LOG_ERR("KOSync", "%s: OOM allocating HTTP client", tag);
        return r;
      }
    }
    http = s_sessionClient.get();
  }

  // No peer verification (setInsecure), matching upstream. The traffic is still
  // TLS-encrypted, but the server cert is not checked against a pinned root.
  // Pinning KOSYNC_CA_ROOTS_PEM via setCACert was tried first (to keep the old
  // mbedtls cert_pem posture) but wolfSSL rejected the handshake with
  // ASN_NO_SIGNER_E (-188): unlike mbedTLS, wolfSSL's path builder would not
  // trace the server chain to those pinned roots. setInsecure is the working
  // convergence with upstream; revisit only if the sync host must be verified.
  http->setReuse(s_sessionActive);
  http->setInsecure();
  http->setTimeout(10000);
  http->setUserAgent(std::string(DEVICE_NAME) + "-ESP32");
  // HTTP Basic Auth for Calibre-Web-Automated compatibility.
  http->setBasicAuth(KOREADER_STORE.getUsername(), KOREADER_STORE.getPassword());

  if (!http->begin(url)) {
    LOG_ERR("KOSync", "%s: malformed URL %s", tag, url.c_str());
    koTraceResp(tag, -1, 0, startMs);
    return r;
  }
  // KOSync auth headers (re-added per request: begin() clears the header list).
  http->addHeader("Accept", "application/vnd.koreader.v1+json");
  http->addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http->addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  if (body) http->addHeader("Content-Type", "application/json");

  // Response sink with two guards. The stats GET aggregates every device's blob, so
  // the body scales with device count:
  //  - overCap: reject once past kMaxResponseBytes (the old ResponseBuffer cap).
  //  - lowHeap: std::string::append reallocates through the throwing global
  //    operator new, which under -fno-exceptions calls abort() (reboot) when the
  //    grown capacity has no contiguous block — exactly the STATS_GET crash the
  //    removed heapOkForUrl gate used to prevent. Reserve once to Content-Length
  //    when known (a single alloc, no doubling), and refuse any growth the heap
  //    can't supply — a clean transport abort (getStats -> NETWORK_ERROR, the leg
  //    is skipped) instead of a crash.
  bool overCap = false;
  bool lowHeap = false;
  bool reserved = false;
  const auto sink = [&r, &overCap, &lowHeap, &reserved, http](const uint8_t* data, size_t len) {
    if (r.body.size() + len > static_cast<size_t>(kMaxResponseBytes)) {
      overCap = true;
      return false;
    }
    // First chunk: reserve the whole body at once when Content-Length is known and
    // the block exists, so later appends never reallocate.
    if (!reserved) {
      reserved = true;
      const size_t cl = http->hasContentLength() ? http->getContentLength() : 0;
      size_t want = cl < static_cast<size_t>(kMaxResponseBytes) ? cl : static_cast<size_t>(kMaxResponseBytes);
      if (want > r.body.capacity()) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_8BIT);
        // Flat headroom, NOT responseHeapMargin. This is the first chunk: r.body is still
        // empty, so reserve(want) is one fresh allocation with no old block held alongside
        // it — the proportional margin below exists to cover libstdc++ keeping the old
        // buffer alive during a growth copy, and there is no copy here. Charging it anyway
        // made every response demand twice its own size: device logs show the per-document
        // STATS_GET (~5667 bytes, so 11334 required) aborting four times while the global
        // one (2569 bytes, 5138 required) succeeded on the same heap.
        if (info.largest_free_block < want + kReserveHeadroom) {
          lowHeap = true;
          return false;
        }
        r.body.reserve(want);
      }
    }
    // Chunked / unknown-length: guard each reallocation. libstdc++ grows to
    // max(needed, 2*capacity), so require that block before letting append run.
    if (r.body.size() + len > r.body.capacity()) {
      const size_t needed = r.body.size() + len;
      const size_t dbl = r.body.capacity() * 2;
      const size_t growTo = needed > dbl ? needed : dbl;
      multi_heap_info_t info;
      heap_caps_get_info(&info, MALLOC_CAP_8BIT);
      if (info.largest_free_block < growTo + responseHeapMargin(growTo)) {
        lowHeap = true;
        return false;
      }
    }
    r.body.append(reinterpret_cast<const char*>(data), len);
    return true;
  };

  if (body) {
    r.status = http->sendRequest(method, reinterpret_cast<const uint8_t*>(body->data()), body->size(), sink);
  } else {
    r.status = http->GET(sink);
  }
  // A guard-tripped transfer is a failure, not a partial success: force a transport
  // error so callers (getStats/getBookmarks) fall to their NETWORK_ERROR path rather
  // than parse a truncated body.
  if (overCap || lowHeap) {
    SdDebugLog::log("KOSYNC", "%s: response %s -> abort clean (bytes=%u)", tag, overCap ? "over-cap" : "low-heap",
                    (unsigned)r.body.size());
    LOG_ERR("KOSync", "%s: %s response, aborting to avoid OOM", tag, overCap ? "over-cap" : "low-heap");
    r.status = -1;
    r.heapAbort = true;
    std::string().swap(r.body);
  }
  r.transportOk = r.status > 0;
  KOReaderSyncClient::lastHttpCode = r.transportOk ? r.status : 0;

  if (body) s_bytesUp += static_cast<uint32_t>(body->size());
  s_bytesDown += static_cast<uint32_t>(r.body.size());
  koTraceResp(tag, r.status, r.body.size(), startMs);
  return r;
}
}  // namespace

// RAII guard (declared in the header): open a keep-alive session for its lifetime.
// Anonymous-namespace helpers above remain visible at file scope in this TU.
KOReaderSyncClient::SyncSession::SyncSession() { beginSession(); }
KOReaderSyncClient::SyncSession::~SyncSession() { endSession(); }

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  const KoResponse resp = koPerform("GET", url, nullptr, "AUTH");
  LOG_DBG("KOSync", "Auth response: %d", resp.status);

  if (!resp.transportOk) return NETWORK_ERROR;
  if (resp.status == 200) return OK;
  if (resp.status == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";

  std::string body;
  {
    JsonDocument doc;
    doc["username"] = KOREADER_STORE.getUsername();
    doc["password"] = KOREADER_STORE.getMd5Password();
    serializeJson(doc, body);
  }

  const KoResponse resp = koPerform("POST", url, &body, "CREATE_USER");
  LOG_DBG("KOSync", "Create user response: %d", resp.status);

  if (!resp.transportOk) return NETWORK_ERROR;
  if (resp.status == 200 || resp.status == 201) return OK;
  if (resp.status == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  const KoResponse resp = koPerform("GET", url, nullptr, "PROGRESS_GET");
  LOG_DBG("KOSync", "Get progress response: %d", resp.status);

  if (!resp.transportOk) return NETWORK_ERROR;

  if (resp.status == 200 && !resp.body.empty()) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, resp.body.c_str());

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    // Extended crosspoint-sync field; absent on plain kosync servers.
    outProgress.position.reset();
    const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
    if (!pos.isNull()) {
      KOReaderRichPosition rich;
      rich.pctQ = pos["pctQ"].as<uint32_t>();
      rich.spineIndex = pos["spine"].as<uint16_t>();
      rich.pageNumber = pos["page"].as<uint16_t>();
      const uint16_t pages = pos["pages"].as<uint16_t>();
      rich.totalPages = pages > 0 ? pages : 1;
      const uint16_t para = pos["para"].as<uint16_t>();
      if (para > 0) rich.paragraphIndex = para;
      rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
      LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
              rich.totalPages, para);
      outProgress.position = std::move(rich);
    }

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  if (resp.status == 401) return AUTH_FAILED;
  if (resp.status == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";

  // Build JSON body. Scope the JsonDocument so its elastic pool is freed before the
  // request (keeps the transient heap tight; harmless with wolfSSL and matches the
  // rest of the file).
  std::string body;
  {
    JsonDocument doc;
    doc["document"] = progress.document;
    // Optional document metadata (KOReader PR #15306; gated by per-server sendMetadata).
    if (progress.metadata.has_value()) {
      auto meta = doc["metadata"].to<JsonObject>();
      meta["filename"] = progress.metadata->filename;
      meta["title"] = progress.metadata->title;
      meta["authors"] = progress.metadata->authors;
    }
    doc["progress"] = progress.progress;
    doc["percentage"] = progress.percentage;
    doc["device"] = DEVICE_NAME;
    doc["device_id"] = KOReaderSyncClient::deviceId();
    if (progress.position.has_value()) {
      // Extended crosspoint-sync field; kosync servers ignore unknown keys.
      const auto& p = *progress.position;
      auto pos = doc["position"].to<JsonObject>();
      pos["pctQ"] = p.pctQ;
      pos["spine"] = p.spineIndex;
      pos["page"] = p.pageNumber;
      pos["pages"] = p.totalPages;
      if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
      // Server rejects the whole position object if xpath exceeds 120 bytes.
      if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
    }
    serializeJson(doc, body);
  }

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  const KoResponse resp = koPerform("PUT", url, &body, "PROGRESS_PUT");
  LOG_DBG("KOSync", "Update progress response: %d", resp.status);

  if (!resp.transportOk) return NETWORK_ERROR;
  if (resp.status == 200 || resp.status == 202) return OK;
  if (resp.status == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getBookmarks(const std::string& documentHash,
                                                           std::string& outBookmarksJson) {
  lastHttpCode = 0;
  outBookmarksJson.clear();
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/bookmarks/" + documentHash;
  const KoResponse resp = koPerform("GET", url, nullptr, "BOOKMARKS_GET");
  LOG_DBG("KOSync", "Get bookmarks response: %d", resp.status);

  if (!resp.transportOk) return NETWORK_ERROR;

  if (resp.status == 200 && !resp.body.empty()) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, resp.body.c_str());
    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    // The server returns {} (no "bookmarks" field) when nothing is stored yet.
    if (!doc["bookmarks"].is<const char*>()) {
      return NOT_FOUND;
    }
    outBookmarksJson = doc["bookmarks"].as<std::string>();
    LOG_DBG("KOSync", "Got bookmarks blob (%u bytes)", (unsigned)outBookmarksJson.size());
    return OK;
  }

  if (resp.status == 401) return AUTH_FAILED;
  if (resp.status == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateBookmarks(const std::string& documentHash,
                                                              const std::string& bookmarksJson) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/bookmarks";

  // The bookmarks array is sent as a single pre-serialized JSON string field so the
  // server stores it as an opaque blob (it never parses bookmark contents).
  std::string body;
  {
    JsonDocument doc;
    doc["document"] = documentHash;
    doc["bookmarks"] = bookmarksJson;

    // Abort-safety (TLS-independent): build the body in ONE pre-sized alloc instead of
    // letting serializeJson grow `body` by doubling reallocs. measureJson() allocates
    // nothing, so the exact length is known; a fragmented heap that can't place the
    // reserve would abort() under -fno-exceptions, so convert that into a clean
    // LOW_MEMORY skip instead.
    const size_t bodyLen = measureJson(doc) + 1;
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    if (info.largest_free_block < bodyLen) {
      SdDebugLog::log("KOSYNC", "BOOKMARKS_PUT body-build: largest=%u < need=%u -> SKIP",
                      (unsigned)info.largest_free_block, (unsigned)bodyLen);
      LOG_ERR("KOSync", "BOOKMARKS_PUT: largest block %u < %u for body build - skip to avoid abort",
              (unsigned)info.largest_free_block, (unsigned)bodyLen);
      return LOW_MEMORY;
    }
    body.reserve(bodyLen);
    serializeJson(doc, body);
  }

  // A dropped bookmark PUT means a local delete never reaches the server, so other
  // devices never converge. Retry the whole request a few times with a settle delay
  // so a transient connect failure can recover.
  constexpr int kMaxAttempts = 3;
  int status = 0;
  bool transportOk = false;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      LOG_DBG("KOSync", "Retrying bookmark upload (attempt %d/%d)", attempt + 1, kMaxAttempts);
      vTaskDelay(pdMS_TO_TICKS(800));
    }
    const KoResponse resp = koPerform("PUT", url, &body, "BOOKMARKS_PUT");
    status = resp.status;
    transportOk = resp.transportOk;
    LOG_DBG("KOSync", "Update bookmarks response: %d (attempt %d)", status, attempt + 1);
    if (transportOk) break;  // got an HTTP response — no point retrying the transport
    if (resp.heapAbort) {
      // Not a transient failure: the guards abandoned the transfer for want of contiguous
      // heap, and nothing between attempts changes that. Device logs show all three attempts
      // aborting at an identical largest=10740 across 2.8s, each paying a full TLS handshake
      // for the same outcome — and each handshake fragments the heap a little further for the
      // legs that follow. Stop and report, rather than spending the remaining budget.
      SdDebugLog::log("KOSYNC", "BOOKMARKS_PUT: heap abort on attempt %d -> no retry (largest=%u)", attempt + 1,
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      LOG_ERR("KOSync", "BOOKMARKS_PUT: heap abort, retrying cannot help - giving up after attempt %d", attempt + 1);
      break;
    }
  }

  if (!transportOk) return NETWORK_ERROR;
  if (status == 200 || status == 202) return OK;
  if (status == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getStats(const std::string& documentHash, KOReaderStatsEntry* outEntries,
                                                       size_t& outCount, const StatsDatedFold* fold,
                                                       const StatsDatedFold* dictFold, const StatsDatedFold* fcFold) {
  lastHttpCode = 0;
  outCount = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/stats/" + documentHash;
  const KoResponse resp = koPerform("GET", url, nullptr, "STATS_GET");
  LOG_DBG("KOSync", "Get stats response: %d", resp.status);

  if (!resp.transportOk) return NETWORK_ERROR;

  if (resp.status == 200 && !resp.body.empty()) {
    JsonDocument doc;
    // Do NOT "optimise" this into the mutable-buffer overload. ArduinoJson 6's zero-copy mode
    // is gone in 7: measured against 7.4.2 with a counting allocator, a 2541-byte stats body
    // peaks at exactly 7474 bytes whether it is parsed from `c_str()` or from a mutable
    // `char*`. The overload only buys a subtler contract (the body is mutated in place), for
    // no memory at all.
    const DeserializationError error = deserializeJson(doc, resp.body.c_str());
    if (error) {
      // SD-only: this is one of three ways `others=` silently reads 0 while every leg
      // logs code=200. NoMemory is the interesting one — the body is still resident here
      // and the document pool is taken on top of it, so a fetch that parsed fine on a
      // stats-only sync can fail on a full-scope one at the same body size.
      SdDebugLog::log("KOSYNC", "STATS_GET: body parse FAILED (%s) bytes=%u free=%u largest=%u", error.c_str(),
                      (unsigned)resp.body.size(), (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    // The server returns {} (no "stats" object) when nothing is stored yet.
    if (!doc["stats"].is<JsonObjectConst>()) {
      // The caller folds NOT_FOUND into statsFetchOk, so this lands as others=0 with
      // fetch=1 — indistinguishable in the log from a successful empty read. A tiny body
      // here is the legitimate "server has nothing yet"; a large one means we received
      // stats and failed to recognise their shape, which is a different problem entirely.
      SdDebugLog::log("KOSYNC", "STATS_GET: no \"stats\" object in %u-byte body -> others will read 0",
                      (unsigned)resp.body.size());
      return NOT_FOUND;
    }

    // One reusable decode buffer for the streaming dated-history fold (cap+stream:
    // decode -> fold -> reuse, so transient heap stays bounded regardless of how
    // many devices carry an "h" section). Allocated only when a fold is requested.
    std::unique_ptr<uint8_t[]> datedBuf;
    if (fold && fold->fn) {
      datedBuf = makeUniqueNoThrow<uint8_t[]>(kStatsDatedMaxBytes);
      if (!datedBuf) LOG_ERR("KOSync", "OOM: dated fold buffer (%u)", (unsigned)kStatsDatedMaxBytes);
    }
    // Separate reusable decode buffer for the per-book dictionary-history "dh" fold.
    std::unique_ptr<uint8_t[]> dictBuf;
    if (dictFold && dictFold->fn) {
      dictBuf = makeUniqueNoThrow<uint8_t[]>(kStatsDictMaxBytes);
      if (!dictBuf) LOG_ERR("KOSync", "OOM: dict fold buffer (%u)", (unsigned)kStatsDictMaxBytes);
    }
    // Separate reusable decode buffer for the per-book flashcard "fc" fold.
    std::unique_ptr<uint8_t[]> fcBuf;
    if (fcFold && fcFold->fn) {
      fcBuf = makeUniqueNoThrow<uint8_t[]>(kStatsFcMaxBytes);
      if (!fcBuf) LOG_ERR("KOSync", "OOM: fc fold buffer (%u)", (unsigned)kStatsFcMaxBytes);
    }

    // Admit only the keys this call will actually read. Without a filter, deserializeJson()
    // materialises the WHOLE blob -- including the base64 "h"/"dh"/"fc" payloads -- into a
    // second document held alongside the still-resident body and outer document, and hits
    // NoMemory. Not hypothetical: a device log showed
    // `dev=1 skipOom=1 bytes=5667 free=33068 largest=8180`, i.e. the other device's entry
    // silently dropped, surfacing to the user as others=0 next to fetch=1. The folds are the
    // only consumers of those three sections, so when a fold is off the payload is decoded
    // purely to be discarded.
    //
    // Measured against ArduinoJson 7.4.2 with a counting allocator, on a blob carrying 4000
    // bytes of "dh" plus 2000 of "fc": 10173 bytes peak unfiltered, 4142 filtered. 4142 is the
    // floor -- an empty 35-byte blob costs the same, because it is ArduinoJson's initial pool
    // chunk -- so the filter removes the payload cost entirely and nothing more. Worth knowing
    // when reading a future skipOom: below ~4.2 KB of contiguous heap this parse cannot
    // succeed at all, whatever the blob looks like.
    //
    // Built once here, not per device: the filter document is reusable and re-creating it
    // inside the loop would re-allocate on every iteration.
    JsonDocument blobFilter;
    blobFilter["s"] = true;
    blobFilter["lr"] = true;
    blobFilter["lh"] = true;
    blobFilter["lm"] = true;
    if (datedBuf) blobFilter["h"] = true;
    if (dictBuf) blobFilter["dh"] = true;
    if (fcBuf) blobFilter["fc"] = true;

    // Every `continue` in this loop drops one device's seconds from the caller's `others`
    // sum with no error return, so a fully-skipped loop is reported as others=0 alongside
    // fetch=1. Counted here and logged below rather than left as per-device LOG_DBGs the
    // device can't emit (USB-locked X3 has no serial).
    uint16_t skipOom = 0;   // blob parse hit NoMemory: a heap symptom, not a data one
    uint16_t skipBad = 0;   // blob was genuinely malformed
    uint16_t skipNull = 0;  // value wasn't a string at all
    uint16_t skipOver = 0;  // past MAX_STATS_DEVICES
    for (JsonPairConst kv : doc["stats"].as<JsonObjectConst>()) {
      if (outCount >= MAX_STATS_DEVICES) {
        skipOver++;
        LOG_DBG("KOSync", "More than %u stats devices; extras dropped", (unsigned)MAX_STATS_DEVICES);
        continue;  // keep counting so the log reports how many were dropped, not just that some were
      }
      // Each value is a per-device blob stored verbatim by the server: an embedded
      // JSON string like {"s":300,"lr":9650,"lh":21,"lm":15} (the global pseudo-doc
      // also carries an "h":"<base64>" dated-history section).
      const char* blob = kv.value().as<const char*>();
      if (!blob) {
        skipNull++;
        continue;
      }
      JsonDocument blobDoc;
      if (const DeserializationError blobErr =
              deserializeJson(blobDoc, blob, DeserializationOption::Filter(blobFilter))) {
        // NoMemory here is the case worth separating: with the filter above the retained part
        // of the blob is a handful of scalars, so failing to parse it means the heap is
        // exhausted, not that the server sent junk. Treating the two alike is what let a heap
        // problem present as "this device has read for 0 seconds".
        if (blobErr == DeserializationError::NoMemory) {
          skipOom++;
        } else {
          skipBad++;
        }
        LOG_DBG("KOSync", "Skipping stats blob for %s: %s", kv.key().c_str(), blobErr.c_str());
        continue;
      }
      KOReaderStatsEntry& e = outEntries[outCount];
      snprintf(e.deviceId, sizeof(e.deviceId), "%s", kv.key().c_str());
      e.seconds = blobDoc["s"].as<uint32_t>();
      e.lastReadDayIndex = blobDoc["lr"].as<uint32_t>();
      e.lastReadHour = blobDoc["lh"].as<uint8_t>();
      e.lastReadMinute = blobDoc["lm"].as<uint8_t>();
      outCount++;

      // Fold this device's dated history (OTHER devices only — local history is the
      // source of truth and is uploaded, not merged back in).
      const bool isOther = strcmp(kv.key().c_str(), deviceId()) != 0;
      if (datedBuf && isOther) {
        const char* hb64 = blobDoc["h"].as<const char*>();
        if (hb64 && hb64[0]) {
          size_t dlen = 0;
          const int rc = mbedtls_base64_decode(datedBuf.get(), kStatsDatedMaxBytes, &dlen,
                                               reinterpret_cast<const unsigned char*>(hb64), strlen(hb64));
          if (rc == 0 && dlen > 0) {
            fold->fn(fold->ctx, datedBuf.get(), dlen);
          } else {
            LOG_DBG("KOSync", "Skipping bad dated blob for %s (rc=%d)", kv.key().c_str(), rc);
          }
        }
      }
      // Fold this device's dictionary history ("dh", OTHER devices only).
      if (dictBuf && isOther) {
        const char* dhb64 = blobDoc["dh"].as<const char*>();
        if (dhb64 && dhb64[0]) {
          size_t dlen = 0;
          const int rc = mbedtls_base64_decode(dictBuf.get(), kStatsDictMaxBytes, &dlen,
                                               reinterpret_cast<const unsigned char*>(dhb64), strlen(dhb64));
          if (rc == 0 && dlen > 0) {
            dictFold->fn(dictFold->ctx, dictBuf.get(), dlen);
          } else {
            LOG_DBG("KOSync", "Skipping bad dict blob for %s (rc=%d)", kv.key().c_str(), rc);
          }
        }
      }
      // Fold this device's flashcard deck ("fc", OTHER devices only).
      if (fcBuf && isOther) {
        const char* fcb64 = blobDoc["fc"].as<const char*>();
        if (fcb64 && fcb64[0]) {
          size_t dlen = 0;
          const int rc = mbedtls_base64_decode(fcBuf.get(), kStatsFcMaxBytes, &dlen,
                                               reinterpret_cast<const unsigned char*>(fcb64), strlen(fcb64));
          if (rc == 0 && dlen > 0) {
            fcFold->fn(fcFold->ctx, fcBuf.get(), dlen);
          } else {
            LOG_DBG("KOSync", "Skipping bad fc blob for %s (rc=%d)", kv.key().c_str(), rc);
          }
        }
      }
    }
    LOG_DBG("KOSync", "Got stats for %u device(s)", (unsigned)outCount);
    // Always logged, not just on skips: `dev=` is what tells a genuine others=0 (one
    // device on the server — ours) apart from a decode that dropped everyone. Heap is
    // sampled here because the fold buffers above are still resident, which is the state
    // the per-device parses actually ran in, not the roomier one at request time.
    SdDebugLog::log(
        "KOSYNC", "STATS_GET decode: dev=%u skipOom=%u skipBad=%u skipNull=%u skipOver=%u bytes=%u free=%u largest=%u",
        (unsigned)outCount, skipOom, skipBad, skipNull, skipOver, (unsigned)resp.body.size(),
        (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return OK;
  }

  if (resp.status == 401) return AUTH_FAILED;
  if (resp.status == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateStats(const std::string& documentHash,
                                                          const KOReaderStatsEntry& entry, const uint8_t* dated,
                                                          size_t datedLen, const uint8_t* dict, size_t dictLen,
                                                          const uint8_t* fc, size_t fcLen) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  statsServerTagBuf[0] = '\0';  // reset; refilled below if the server echoes a tag

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/stats";

  // The per-device counters are sent as a pre-serialized JSON string field so the
  // server stores the blob verbatim under this device's hash field (other devices'
  // blobs are untouched). Optional base64 "h" (dated reading-history), "dh" (per-book
  // dictionary-history) and "fc" (per-book flashcards) sections are spliced in before
  // the closing brace. base64 uses A-Za-z0-9+/= — none need JSON-string escaping.
  std::string statsBlob;  // outlives serializeJson below
  statsBlob.reserve(64);
  {
    char scalar[64];
    const int sn = snprintf(
        scalar, sizeof(scalar), "{\"s\":%lu,\"lr\":%lu,\"lh\":%u,\"lm\":%u}", static_cast<unsigned long>(entry.seconds),
        static_cast<unsigned long>(entry.lastReadDayIndex), entry.lastReadHour, entry.lastReadMinute);
    statsBlob.assign(scalar, sn - 1);  // drop trailing '}'; re-added after the optional fields
  }

  auto appendB64Field = [&statsBlob](const char* name, const uint8_t* data, size_t dataLen) {
    if (!data || dataLen == 0) return;
    size_t encLen = 0;
    mbedtls_base64_encode(nullptr, 0, &encLen, data, dataLen);  // query size (incl NUL)
    auto enc = makeUniqueNoThrow<unsigned char[]>(encLen > 0 ? encLen : 1);
    size_t written = 0;
    if (enc && mbedtls_base64_encode(enc.get(), encLen, &written, data, dataLen) == 0) {
      statsBlob += ",\"";
      statsBlob += name;
      statsBlob += "\":\"";
      statsBlob.append(reinterpret_cast<const char*>(enc.get()), written);
      statsBlob += "\"";
    } else {
      LOG_ERR("KOSync", "Stats base64 encode failed for \"%s\"; field omitted", name);
    }
  };
  appendB64Field("h", dated, datedLen);
  appendB64Field("dh", dict, dictLen);
  appendB64Field("fc", fc, fcLen);
  statsBlob += "}";

  // Build the request body by hand instead of via a JsonDocument, which would add a
  // doubling-growth pool that *copies* statsBlob (a ~5.5KB base64 "dh" blob) — the
  // unguarded throwing-new that aborted on a fragmented heap. Manual concat needs
  // one exact-sized reserve and no large transient.
  //
  // CRITICAL wire format: "stats" is a quoted JSON *string* field (the server stores the
  // blob verbatim — see the comment above where statsBlob is built), NOT a nested object.
  // statsBlob is itself serialized JSON, so it must be JSON-escaped here. base64 + the
  // scalar JSON only ever contain '"' (never '\'), but escape both for safety. documentHash
  // (hex) and deviceId() ("crosspoint-<12 hex>") are JSON-safe and need no escaping.
  const char* devId = deviceId();
  const size_t devLen = strlen(devId);
  size_t statsEsc = 0;  // one scan, no allocation -> the reserve below stays exact
  for (char ch : statsBlob)
    if (ch == '"' || ch == '\\') statsEsc++;
  // Fixed punctuation: {"document":"  ","device_id":"  ","stats":"  "} = 42 bytes.
  const size_t bodyLen = 42 + documentHash.size() + devLen + statsBlob.size() + statsEsc + 1;  // +1 NUL headroom

  // Abort-safety (TLS-independent): the reserve below is the only sizable alloc. Check
  // contiguous heap first; starved -> clean LOW_MEMORY skip instead of throwing-new abort().
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  if (info.largest_free_block < bodyLen) {
    SdDebugLog::log("KOSYNC", "STATS_PUT body-build: largest=%u < need=%u -> SKIP", (unsigned)info.largest_free_block,
                    (unsigned)bodyLen);
    LOG_ERR("KOSync", "STATS_PUT: largest block %u < %u for body build - skip to avoid abort",
            (unsigned)info.largest_free_block, (unsigned)bodyLen);
    std::string().swap(statsBlob);
    return LOW_MEMORY;
  }

  std::string body;
  body.reserve(bodyLen);
  body = "{\"document\":\"";
  body += documentHash;
  body += "\",\"device_id\":\"";
  body += devId;
  body += "\",\"stats\":\"";  // stats is a quoted, JSON-escaped string field
  for (char ch : statsBlob) {
    if (ch == '"' || ch == '\\') body += '\\';
    body += ch;
  }
  body += "\"}";
  std::string().swap(statsBlob);  // free the base64 blob's heap before the request

  LOG_DBG("KOSync", "Stats request body: %s", body.c_str());

  const KoResponse resp = koPerform("PUT", url, &body, "STATS_PUT");
  LOG_DBG("KOSync", "Update stats response: %d", resp.status);

  // No retry loop (unlike updateBookmarks): the counters are monotonic and re-sent
  // whole on every sync, so a dropped PUT self-heals next time — nothing diverges.
  if (!resp.transportOk) return NETWORK_ERROR;
  if (resp.status == 200 || resp.status == 202) {
    // The stats-enabled server echoes a capability tag ("server":"stats-v1") in
    // its response; surface it so the UI can show which server build answered.
    if (!resp.body.empty()) {
      JsonDocument respDoc;
      if (!deserializeJson(respDoc, resp.body.c_str()) && respDoc["server"].is<const char*>()) {
        snprintf(statsServerTagBuf, sizeof(statsServerTagBuf), "%s", respDoc["server"].as<const char*>());
      }
    }
    return OK;
  }
  if (resp.status == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::statsServerTag() { return statsServerTagBuf; }

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case LOW_MEMORY:
      return "Not enough memory for HTTPS sync. Open a book and sync from the reader instead.";
    default:
      return "Unknown error";
  }
}
