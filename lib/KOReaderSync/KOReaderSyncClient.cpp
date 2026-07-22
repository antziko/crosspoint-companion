#include "KOReaderSyncClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <ctime>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncCA.h"

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
// timeout (ESP_ERR_HTTP_CONNECT) — observed on the X3, where even the first
// KOSync GET never connected while the X4 (more headroom) merely ran slow.
// HttpDownloader already does this for OPDS/downloads (HttpDownloader.cpp:79);
// KOSync went through the default and paid for it. RAII so every return path
// restores power-save. See HttpDownloader's NoWifiSleep.
struct NoWifiSleep {
  NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

// Small TLS buffers to fit in ESP32-C3's limited heap (~46KB free after WiFi).
// KOSync payloads are tiny JSON (<1KB), so 2KB buffers are sufficient.
// Default 16KB buffers cause OOM during TLS handshake.
constexpr int HTTP_BUF_SIZE = 2048;

// Cloudflare tunnels send a 3-cert Google Trust Services chain. During the TLS handshake
// mbedTLS makes many small allocations that collectively consume ~48KB of heap. With only
// ~50KB free after WiFi connects, the session drove min-free-ever down to 2600 bytes before
// failing with MBEDTLS_ERR_X509_ALLOC_FAILED (-0x2880). Check total free heap (not max
// contiguous block) because the failure mode is aggregate exhaustion, not one large alloc.
//
// On X3 in settings context after WiFi, only ~53KB is free — below this threshold.
// Auth-from-settings will show LOW_MEMORY on X3 with HTTPS servers. The workaround is
// to sync from within the reader, which releases the epub first and frees enough RAM.
// Tested down to 50000 (X4, settings-context, kosync.yapaa.org Cloudflare tunnel): even with
// ~50.9KB free after the inflate-window release, mbedtls_ssl_setup failed with SSL_ALLOC_FAILED
// (-0x7F00). The killer is contiguous space, not total free: the SSL in_buf + out_buf each need
// ~16KB contiguous, and after the first carves the lone 32KB block the second can't fit. So the
// real bar is two 16KB slabs, which settings-context (no epub to release) can't supply. Keep the
// guard high enough to fail fast with a clean LOW_MEMORY ("sync from the reader") message instead
// of an mbedTLS connect error. Reader-context sync frees ~65KB epub and handshakes fine.
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// Plain HTTP does no TLS handshake, so it never allocates the mbedTLS arena. It
// needs only the fixed rx/tx buffers (HTTP_BUF_SIZE each), the esp_http_client
// struct, and a small JSON doc — a few KB. Gate HTTP requests on this far lower
// bar so a local http:// sync server isn't rejected by the TLS-sized guard.
constexpr uint32_t MIN_HEAP_FOR_HTTP = 12000;

// Coarse contiguous backstop for HTTPS PUTs (see the two-16KB-slab note above). Evaluated
// AFTER the body is serialized: ssl_setup carves a second ~16KB record slab, and a
// multi-KB request body competing for it tips ssl_setup into -0x7F00. We can observe only
// the *largest* free block, not the second, so this is a coarse guard.
//
// RECALIBRATED 2026-06-22 for the SHRUNK mbedTLS record buffers. The custom libmbedtls_2.a
// (esp32-arduino-lib-builder, ASYMMETRIC_CONTENT_LEN=y, IN=8192/OUT=4096 — see
// docs/kosync-https-libbuilder.md) drops the handshake record buffers from ~16.7 KB to
// in_buf ~8.5 KB + out_buf ~4.4 KB. The binding contiguous constraint is now the IN buffer
// (~8.5 KB); the OUT buffer is a separate, smaller alloc that takes any of the dozens of free
// blocks. The old base (18000, sized for the 16 KB IN slab) FALSE-SKIPS PUTs that now succeed —
// hardware evidence 2026-06-22: a sync-all STATS_PUT (body 5299) was rejected at largest=15348
// with need=23299, yet the handshake needs only ~8.5 KB contiguous and would have completed.
// Set the base to IN buffer (~8.5 KB) + margin. A still-fragmented arena that can't place the
// IN slab fails ssl_setup (-0x7F00) and the PUT skips gracefully (no crash) — better than never
// attempting. If the buffers are resized in the lib build, retune this.
constexpr uint32_t TLS_PUT_CONTIG_BASE = 10000;

// X3 HTTPS troubleshooting instrumentation (SdDebugLog "STALL"): a gap between
// esp_http_client event-callback fires longer than this is logged with a
// heap+RSSI snapshot. Mirrors HttpDownloader::runGet's per-chunk-read probe so
// the perform()-based GET/PUT path here produces directly comparable evidence
// to the streaming-GET path. See SUMMARY.md Part B Appendix.
constexpr uint32_t STALL_LOG_THRESHOLD_MS = 1000;

// Response buffer for reading HTTP body
struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  // Set by beginTrace() before perform(); read by httpEventHandler to log
  // connect-time and stall events to SD. Null traceTag = no tracing (cheap).
  const char* traceTag = nullptr;
  uint32_t requestStartMs = 0;
  uint32_t lastEventMs = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size <= capacity) return true;
    if (size > kMaxResponseBytes) return false;  // reject pathological responses, don't grow unbounded
    char* newData = (char*)realloc(data, size);
    if (!newData) return false;
    data = newData;
    capacity = size;
    return true;
  }
};

// HTTP event handler: collects the response body, and (when traceTag is set)
// logs TLS-connect duration and any stall between successive data events.
esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (!buf) return ESP_OK;

  if (buf->traceTag) {
    const uint32_t now = millis();
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
      // Snapshot right after the TLS handshake: this is where the mbedTLS arena
      // (CA bundle parse + record buffers) has just been carved out of the heap,
      // so it shows the post-handshake headroom the body read has to live in.
      const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
      SdDebugLog::log("KOSYNC", "%s TLS connected after %lums heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d",
                      buf->traceTag, (unsigned long)(now - buf->requestStartMs), snap.heapFree, snap.largest8Bit,
                      snap.internalFree, snap.internalLargest, (int)snap.rssi);
      buf->lastEventMs = now;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
      const uint32_t gapMs = now - buf->lastEventMs;
      if (gapMs > STALL_LOG_THRESHOLD_MS) {
        const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
        SdDebugLog::log("STALL", "%s gap=%lums bytes=%d heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d",
                        buf->traceTag, (unsigned long)gapMs, buf->len, snap.heapFree, snap.largest8Bit,
                        snap.internalFree, snap.internalLargest, (int)snap.rssi);
      }
      buf->lastEventMs = now;
    }
  }

  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (buf->ensure(buf->len + evt->data_len + 1)) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    } else {
      LOG_ERR("KOSync", "Response buffer allocation failed (%d bytes)", evt->data_len);
    }
  }
  return ESP_OK;
}

// --- Failed-allocation instrumentation (dev diagnostic, 2026-06-21) ---
// The HTTPS handshake fast-fails (ESP_ERR_HTTP_CONNECT in ~64ms, no "TLS connected") when the
// largest free block is ~31-32K but succeeds (~1589ms real handshake) at ~34K — implying ONE
// large contiguous malloc in esp_tls/mbedtls setup is failing. The shipped prebuilt mbedtls is
// already IN=8192/OUT=2048 asymmetric (record buffers ~10K total), so the ~33K culprit is NOT the
// OUT content buffer and lib-building OUT=2048 would be pointless. This hook records the size of
// the failing alloc so the real culprit can be identified BEFORE committing to a lib-builder
// rebuild. The callback runs in the failing allocator's task context (esp_tls task here): it does
// only scalar writes to DRAM statics — NO allocation, NO SD I/O — so it is reentrancy-safe. The
// captured values are flushed to SD from endTrace (task context, safe).
struct FailedAllocCapture {
  volatile uint32_t count;
  volatile uint32_t maxSize;
  volatile uint32_t lastSize;
  volatile uint32_t caps;
};
FailedAllocCapture s_failAlloc = {0, 0, 0, 0};

void allocFailHook(size_t size, uint32_t caps, const char* /*function_name*/) {
  s_failAlloc.count++;
  s_failAlloc.lastSize = static_cast<uint32_t>(size);
  if (size > s_failAlloc.maxSize) s_failAlloc.maxSize = static_cast<uint32_t>(size);
  s_failAlloc.caps = caps;
}

// Register the global failed-alloc hook once. Only fires on a FAILED allocation, so the
// steady-state overhead is zero. No public unregister API; left installed for the session.
void ensureAllocHook() {
  static bool registered = false;
  if (registered) return;
  if (heap_caps_register_failed_alloc_callback(allocFailHook) == ESP_OK) registered = true;
}

void resetFailAlloc() {
  s_failAlloc.count = 0;
  s_failAlloc.maxSize = 0;
  s_failAlloc.lastSize = 0;
  s_failAlloc.caps = 0;
}

// Flush the per-request failed-alloc capture. caps bit0=MALLOC_CAP_EXEC, and for our purpose
// the key signal is maxSize: a single ~33K maxSize confirms one large contiguous alloc is the
// wall (lib-builder DYNAMIC_BUFFER is then the lever); many small fails would mean fragmentation.
void dumpFailAlloc(const char* tag) {
  if (s_failAlloc.count == 0) return;
  SdDebugLog::log("FAILALLOC", "%s fails=%u maxSize=%u lastSize=%u caps=0x%x", tag, (unsigned)s_failAlloc.count,
                  (unsigned)s_failAlloc.maxSize, (unsigned)s_failAlloc.lastSize, (unsigned)s_failAlloc.caps);
  LOG_ERR("KOSync", "%s: %u failed alloc(s), max=%u bytes - see FAILALLOC line", tag, (unsigned)s_failAlloc.count,
          (unsigned)s_failAlloc.maxSize);
}

// Logs a pre-request heap+RSSI snapshot (see SdDebugLog::NetSnapshot) and arms
// `buf` so httpEventHandler logs connect/stall events under the same tag.
void beginTrace(ResponseBuffer& buf, const char* tag, size_t bodyLen = 0) {
  ensureAllocHook();
  resetFailAlloc();
  buf.traceTag = tag;
  buf.requestStartMs = millis();
  buf.lastEventMs = buf.requestStartMs;
  const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
  SdDebugLog::log("KOSYNC", "%s req body=%u heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d", tag,
                  (unsigned)bodyLen, snap.heapFree, snap.largest8Bit, snap.internalFree, snap.internalLargest,
                  (int)snap.rssi);
}

// Logs the outcome + total elapsed time, in the same {bytes, elapsed, rate}
// shape HttpDownloader's DONE line uses so GET/PUT traces read consistently.
void endTrace(const ResponseBuffer& buf, const char* tag, int httpCode, esp_err_t err) {
  // Include the esp_err name (ESP_ERR_HTTP_CONNECT vs a TLS/alloc error tells
  // transport-fail from handshake-OOM apart) and a post-op heap snapshot taken
  // after cleanup so a leak or non-reclaimed arena across a sync shows up.
  const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
  SdDebugLog::log("KOSYNC",
                  "%s resp code=%d err=%d(%s) elapsed=%lums bytes=%d heap=%u largest8=%u intFree=%u intLargest=%u", tag,
                  httpCode, (int)err, esp_err_to_name(err), (unsigned long)(millis() - buf.requestStartMs), buf.len,
                  snap.heapFree, snap.largest8Bit, snap.internalFree, snap.internalLargest);
  // If any allocation failed during this request (e.g. the esp_tls/mbedtls handshake setup that
  // turns ESP_ERR_HTTP_CONNECT), emit its size so the ~33K contiguous wall can be pinned.
  dumpFailAlloc(tag);
}

// --- Connection reuse (keep-alive) session ---
// The ESP32-C3 cannot do rapid back-to-back NEW TLS connections to the same host:
// LWIP TIME_WAIT / socket churn yields sock<0 and ~16s connect timeouts (the server
// is fine — verified with 8 parallel curls at ~70ms each). Within a session, ONE
// keep-alive connection is reused across every leg of a sync (set_url/set_method per
// request), so there is a single TLS handshake instead of ~7 reconnects.
esp_http_client_handle_t s_sessionClient = nullptr;
bool s_sessionActive = false;

void beginSession() {
  s_sessionActive = true;  // the handle is lazily created on the first createClient()
}

void endSession() {
  if (s_sessionClient) {
    esp_http_client_cleanup(s_sessionClient);
    s_sessionClient = nullptr;
  }
  s_sessionActive = false;
}

// Cleanup unless this is the live session connection (freed once by endSession()).
void releaseClient(esp_http_client_handle_t client) {
  if (s_sessionActive && client == s_sessionClient) return;  // keep the connection alive
  esp_http_client_cleanup(client);
}

// Create configured esp_http_client with small TLS buffers
esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  // Reuse the live session connection if one is established: just retarget it at the
  // new URL/method/response-buffer. No new TCP/TLS handshake -> no reconnect churn.
  if (s_sessionActive && s_sessionClient) {
    if (esp_http_client_set_url(s_sessionClient, url) != ESP_OK ||
        esp_http_client_set_method(s_sessionClient, method) != ESP_OK ||
        esp_http_client_set_user_data(s_sessionClient, buf) != ESP_OK) {
      LOG_ERR("KOSync", "Failed to retarget keep-alive client");
      return nullptr;
    }
    // Clear per-request state from the previous leg so it can't leak forward
    // (e.g. a PUT's body or Content-Type carrying into the next GET).
    esp_http_client_set_post_field(s_sessionClient, nullptr, 0);
    esp_http_client_delete_header(s_sessionClient, "Content-Type");
    return s_sessionClient;
  }

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = buf;
  config.method = method;
  config.timeout_ms = 10000;
  // Keep-alive ONLY inside a session, where the held connection is genuinely reused
  // across legs. For a one-shot request (no session: progress GET, bookmark GET/PUT)
  // negotiating keep-alive makes the server hold the socket open; our immediate
  // esp_http_client_cleanup() then leaves it lingering (TIME_WAIT / half-open), and the
  // next one-shot connect() to the same host collides with it -> "Failed to open a new
  // connection", sock<0, ~11s SYN timeout (the bookmark PUT regression). A one-shot with
  // keep_alive disabled sends Connection: close and releases the socket cleanly.
  config.keep_alive_enable = s_sessionActive;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  // Pin the sync-server roots instead of attaching the full ~16 KB Mozilla bundle.
  // The bundle's contiguous handshake allocation was collapsing the largest free
  // block to ~5 KB on the X4 and failing the bookmark PUT (EAGAIN / ssl_setup
  // -0x7F00). See KOReaderSyncCA.h for the pinned roots and the trade-off.
  config.cert_pem = KOSYNC_CA_ROOTS_PEM;

  // HTTP Basic Auth for Calibre-Web-Automated compatibility
  config.username = KOREADER_STORE.getUsername().c_str();
  config.password = KOREADER_STORE.getPassword().c_str();
  config.auth_type = HTTP_AUTH_TYPE_BASIC;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return nullptr;

  // KOSync auth headers
  if (esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json") != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-user", KOREADER_STORE.getUsername().c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "x-auth-key", KOREADER_STORE.getMd5Password().c_str()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set auth headers");
    esp_http_client_cleanup(client);
    return nullptr;
  }

  if (s_sessionActive) s_sessionClient = client;  // adopt as the reusable session connection
  return client;
}

// Pre-flight the heap for an HTTP(S) request. HTTPS needs the full mbedTLS
// handshake arena; plain HTTP needs only the small fixed buffers. Returns true if
// there is enough free heap, otherwise logs and returns false. The caller maps
// false to LOW_MEMORY. `url` carries the scheme (from getBaseUrl()).
bool heapOkForUrl(const std::string& url, const char* tag) {
  // In-session reuse: a live keep-alive client already holds the mbedTLS arena,
  // so this request retargets it with NO new handshake and needs no fresh heap.
  // The gate's 55KB threshold is for a cold handshake and would wrongly REJECT
  // every leg after the first (the held arena leaves only ~24KB free).
  if (s_sessionActive && s_sessionClient) {
    LOG_DBG("KOSync", "%s: %s (reuse, gate bypassed)", tag, url.c_str());
    return true;
  }
  const bool https = url.rfind("https://", 0) == 0;
  const uint32_t need = https ? MIN_HEAP_FOR_TLS : MIN_HEAP_FOR_HTTP;
  const uint32_t freeHeap = ESP.getFreeHeap();
  LOG_DBG("KOSync", "%s: %s (free=%u, need=%u, %s)", tag, url.c_str(), (unsigned)freeHeap, (unsigned)need,
          https ? "https" : "http");
  // Record the preflight decision to SD: an AUTH/PUT that fails here returns
  // LOW_MEMORY *before* any TLS attempt, so the on-device error ("not enough
  // memory") looks identical to a real handshake failure. This line disambiguates
  // — REJECT means the gate blocked it, not the network.
  {
    const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
    SdDebugLog::log("KOSYNC", "%s gate: free=%u need=%u %s largest8=%u intFree=%u intLargest=%u -> %s", tag,
                    (unsigned)freeHeap, (unsigned)need, https ? "https" : "http", snap.largest8Bit, snap.internalFree,
                    snap.internalLargest, (freeHeap < need) ? "REJECT" : "ok");
  }
  if (freeHeap < need) {
    LOG_ERR("KOSync", "Insufficient heap: %u bytes free (need %u for %s)", freeHeap, need,
            https ? "TLS handshake" : "HTTP");
    return false;
  }
  return true;
}

// Body-aware contiguous backstop for an HTTPS PUT, evaluated AFTER the body is serialized
// (so bodyLen and the real pre-handshake heap topology are known). Returns false ->
// caller maps to LOW_MEMORY and skips, avoiding a guaranteed handshake fast-fail cascade.
// Always emits a rich SD line — free_blocks + total_free vs largest expose whether a
// SECOND large slab exists for out_buf, which the largest8 line alone cannot show.
bool contigOkForPut(const std::string& url, const char* tag, size_t bodyLen) {
  if (url.rfind("https://", 0) != 0) return true;  // plain HTTP: no mbedTLS arena
  // In-session reuse: no fresh handshake -> the out_buf contiguous backstop
  // (sized for a cold handshake) does not apply; the live arena is already up.
  if (s_sessionActive && s_sessionClient) return true;
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  const uint32_t need = TLS_PUT_CONTIG_BASE + static_cast<uint32_t>(bodyLen);
  const bool ok = info.largest_free_block >= need;
  SdDebugLog::log("KOSYNC", "%s contig: largest=%u total_free=%u free_blocks=%u min_free=%u body=%u need=%u -> %s", tag,
                  (unsigned)info.largest_free_block, (unsigned)info.total_free_bytes, (unsigned)info.free_blocks,
                  (unsigned)info.minimum_free_bytes, (unsigned)bodyLen, (unsigned)need, ok ? "ok" : "SKIP");
  if (!ok)
    LOG_ERR("KOSync", "%s: largest block %u < %u needed (body %u) - skip to avoid handshake cascade", tag,
            (unsigned)info.largest_free_block, (unsigned)need, (unsigned)bodyLen);
  return ok;
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

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  if (!heapOkForUrl(url, "AUTH")) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "AUTH");
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  releaseClient(client);

  endTrace(buf, "AUTH", httpCode, err);
  LOG_DBG("KOSync", "Auth response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200) return OK;
  if (httpCode == 401) return AUTH_FAILED;
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

  if (!contigOkForPut(url, "CREATE_USER", body.length())) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "CREATE_USER", body.length());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_POST);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json") != ESP_OK ||
      esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set request body");
    releaseClient(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  releaseClient(client);

  s_bytesUp += static_cast<uint32_t>(body.length());
  endTrace(buf, "CREATE_USER", httpCode, err);
  LOG_DBG("KOSync", "Create user response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 201) return OK;
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  if (!heapOkForUrl(url, "PROGRESS_GET")) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "PROGRESS_GET");
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  releaseClient(client);

  s_bytesDown += static_cast<uint32_t>(buf.len);
  endTrace(buf, "PROGRESS_GET", httpCode, err);
  LOG_DBG("KOSync", "Get progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;

  if (httpCode == 200 && buf.data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, buf.data);

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

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  if (!heapOkForUrl(url, "PROGRESS_PUT")) return LOW_MEMORY;

  // Build JSON body. Scope the JsonDocument so its elastic pool is freed before the
  // TLS handshake — the mbedTLS arena needs two ~16KB *contiguous* buffers, and a live
  // doc fragments the largest block below that, causing ESP_ERR_HTTP_CONNECT on PUT.
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

  if (!contigOkForPut(url, "PROGRESS_PUT", body.length())) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "PROGRESS_PUT", body.length());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set request body");
    releaseClient(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  releaseClient(client);

  s_bytesUp += static_cast<uint32_t>(body.length());
  endTrace(buf, "PROGRESS_PUT", httpCode, err);
  LOG_DBG("KOSync", "Update progress response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
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

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/bookmarks/" + documentHash;
  if (!heapOkForUrl(url, "BOOKMARKS_GET")) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "BOOKMARKS_GET");
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  releaseClient(client);

  s_bytesDown += static_cast<uint32_t>(buf.len);
  endTrace(buf, "BOOKMARKS_GET", httpCode, err);
  LOG_DBG("KOSync", "Get bookmarks response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;

  if (httpCode == 200 && buf.data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, buf.data);
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

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateBookmarks(const std::string& documentHash,
                                                              const std::string& bookmarksJson) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/bookmarks";
  if (!heapOkForUrl(url, "BOOKMARKS_PUT")) return LOW_MEMORY;

  // The bookmarks array is sent as a single pre-serialized JSON string field so the
  // server stores it as an opaque blob (it never parses bookmark contents).
  // Scope the JsonDocument so its pool frees before the TLS handshake: the mbedTLS arena
  // needs two ~16KB *contiguous* buffers, and a live doc (plus this multi-KB body) drops
  // the largest free block below that, causing the back-to-back PUT to fail with
  // ESP_ERR_HTTP_CONNECT (no handshake) — see updateProgress for the same pattern.
  std::string body;
  {
    JsonDocument doc;
    doc["document"] = documentHash;
    doc["bookmarks"] = bookmarksJson;

    // Abort-safety: build the body in ONE pre-sized alloc instead of letting serializeJson
    // grow `body` by doubling reallocs. measureJson() allocates nothing, so the exact body
    // length is known; the largest-free-block check runs AFTER `doc` is built (its pool is
    // already resident), so it reflects the contiguous heap the reserve() will actually need.
    // On a reused session the mbedTLS arena has crushed the largest free block, so a doubling
    // grow would abort() under -fno-exceptions; this converts that into a clean LOW_MEMORY skip
    // (session closes after, heap recovers, retry works). Was the broken `2*blob` guard that
    // ignored the live doc pool — HW crash 2026-06-22: blob=1984, abort on the 3841 realloc.
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

  // The bookmark PUT is the last of several TLS handshakes in a sync, and on some devices the
  // fresh handshake opened right after the bookmark GET's teardown fails to connect
  // (ESP_ERR_HTTP_CONNECT) — a transient back-to-back reconnect issue, not a heap/auth fault.
  // Retry the whole request a few times with a settle delay so the transport can recover. This
  // matters because a dropped PUT means a local delete never reaches the server, so other
  // devices never converge.
  if (!contigOkForPut(url, "BOOKMARKS_PUT", body.length())) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;  // keep the radio awake across all retry attempts
  constexpr int kMaxAttempts = 3;
  esp_err_t err = ESP_FAIL;
  int httpCode = 0;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt > 0) {
      LOG_DBG("KOSync", "Retrying bookmark upload (attempt %d/%d)", attempt + 1, kMaxAttempts);
      vTaskDelay(pdMS_TO_TICKS(800));
    }

    ResponseBuffer buf;
    beginTrace(buf, "BOOKMARKS_PUT", body.length());
    esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
    if (!client) {
      err = ESP_FAIL;
      continue;
    }
    if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
      LOG_ERR("KOSync", "Failed to set request body");
      releaseClient(client);
      err = ESP_FAIL;
      continue;
    }

    err = esp_http_client_perform(client);
    httpCode = esp_http_client_get_status_code(client);
    releaseClient(client);
    endTrace(buf, "BOOKMARKS_PUT", httpCode, err);
    LOG_DBG("KOSync", "Update bookmarks response: %d (err: %d, attempt %d)", httpCode, err, attempt + 1);

    if (err == ESP_OK) break;  // got an HTTP response — no point retrying the transport
  }
  lastHttpCode = httpCode;
  if (err == ESP_OK) s_bytesUp += static_cast<uint32_t>(body.length());
  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
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

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/stats/" + documentHash;
  if (!heapOkForUrl(url, "STATS_GET")) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "STATS_GET");
  esp_http_client_handle_t client = createClient(url.c_str(), &buf);
  if (!client) return NETWORK_ERROR;

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  releaseClient(client);

  s_bytesDown += static_cast<uint32_t>(buf.len);
  endTrace(buf, "STATS_GET", httpCode, err);
  LOG_DBG("KOSync", "Get stats response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;

  if (httpCode == 200 && buf.data) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, buf.data);
    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    // The server returns {} (no "stats" object) when nothing is stored yet.
    if (!doc["stats"].is<JsonObjectConst>()) {
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

    for (JsonPairConst kv : doc["stats"].as<JsonObjectConst>()) {
      if (outCount >= MAX_STATS_DEVICES) {
        LOG_DBG("KOSync", "More than %u stats devices; extras dropped", (unsigned)MAX_STATS_DEVICES);
        break;
      }
      // Each value is a per-device blob stored verbatim by the server: an embedded
      // JSON string like {"s":300,"lr":9650,"lh":21,"lm":15} (the global pseudo-doc
      // also carries an "h":"<base64>" dated-history section).
      const char* blob = kv.value().as<const char*>();
      if (!blob) continue;
      JsonDocument blobDoc;
      if (deserializeJson(blobDoc, blob)) {
        LOG_DBG("KOSync", "Skipping malformed stats blob for %s", kv.key().c_str());
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
    return OK;
  }

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
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

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/stats";
  if (!heapOkForUrl(url, "STATS_PUT")) return LOW_MEMORY;

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
  // unguarded throwing-new that aborted on the fragmented X3 heap. Manual concat needs
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

  // Abort-safety: the reserve below is the only sizable alloc. Check contiguous heap
  // first; starved -> clean LOW_MEMORY skip instead of throwing-new abort().
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
  std::string().swap(statsBlob);  // free the base64 blob's heap before the handshake

  LOG_DBG("KOSync", "Stats request body: %s", body.c_str());

  if (!contigOkForPut(url, "STATS_PUT", body.length())) return LOW_MEMORY;

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "STATS_PUT", body.length());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set request body");
    releaseClient(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  releaseClient(client);

  s_bytesUp += static_cast<uint32_t>(body.length());
  endTrace(buf, "STATS_PUT", httpCode, err);
  LOG_DBG("KOSync", "Update stats response: %d (err: %d)", httpCode, err);

  // No retry loop (unlike updateBookmarks): the counters are monotonic and re-sent
  // whole on every sync, so a dropped PUT self-heals next time — nothing diverges.
  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) {
    // The stats-enabled server echoes a capability tag ("server":"stats-v1") in
    // its response; surface it so the UI can show which server build answered.
    if (buf.data) {
      JsonDocument respDoc;
      if (!deserializeJson(respDoc, buf.data) && respDoc["server"].is<const char*>()) {
        snprintf(statsServerTagBuf, sizeof(statsServerTagBuf), "%s", respDoc["server"].as<const char*>());
      }
    }
    return OK;
  }
  if (httpCode == 401) return AUTH_FAILED;
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
