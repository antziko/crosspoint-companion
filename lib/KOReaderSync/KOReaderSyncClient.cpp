#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SdDebugLog.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

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
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// Plain HTTP does no TLS handshake, so it never allocates the mbedTLS arena. It
// needs only the fixed rx/tx buffers (HTTP_BUF_SIZE each), the esp_http_client
// struct, and a small JSON doc — a few KB. Gate HTTP requests on this far lower
// bar so a local http:// sync server isn't rejected by the TLS-sized guard.
constexpr uint32_t MIN_HEAP_FOR_HTTP = 12000;

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

// Logs a pre-request heap+RSSI snapshot (see SdDebugLog::NetSnapshot) and arms
// `buf` so httpEventHandler logs connect/stall events under the same tag.
void beginTrace(ResponseBuffer& buf, const char* tag, size_t bodyLen = 0) {
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
}

// Create configured esp_http_client with small TLS buffers
esp_http_client_handle_t createClient(const char* url, ResponseBuffer* buf,
                                      esp_http_client_method_t method = HTTP_METHOD_GET) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = httpEventHandler;
  config.user_data = buf;
  config.method = method;
  config.timeout_ms = 15000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

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

  return client;
}

// Pre-flight the heap for an HTTP(S) request. HTTPS needs the full mbedTLS
// handshake arena; plain HTTP needs only the small fixed buffers. Returns true if
// there is enough free heap, otherwise logs and returns false. The caller maps
// false to LOW_MEMORY. `url` carries the scheme (from getBaseUrl()).
bool heapOkForUrl(const std::string& url, const char* tag) {
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
}  // namespace

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
  lastHttpCode = httpCode;  esp_http_client_cleanup(client);

  endTrace(buf, "AUTH", httpCode, err);
  LOG_DBG("KOSync", "Auth response: %d (err: %d)", httpCode, err);

  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200) return OK;
  if (httpCode == 401) return AUTH_FAILED;
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
  lastHttpCode = httpCode;  esp_http_client_cleanup(client);

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

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  const NoWifiSleep noWifiSleep;
  ResponseBuffer buf;
  beginTrace(buf, "PROGRESS_PUT", body.length());
  esp_http_client_handle_t client = createClient(url.c_str(), &buf, HTTP_METHOD_PUT);
  if (!client) return NETWORK_ERROR;

  if (esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_post_field(client, body.c_str(), body.length()) != ESP_OK) {
    LOG_ERR("KOSync", "Failed to set request body");
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;  esp_http_client_cleanup(client);

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
  lastHttpCode = httpCode;  esp_http_client_cleanup(client);

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
  JsonDocument doc;
  doc["document"] = documentHash;
  doc["bookmarks"] = bookmarksJson;

  std::string body;
  serializeJson(doc, body);

  // The bookmark PUT is the last of several TLS handshakes in a sync, and on some devices the
  // fresh handshake opened right after the bookmark GET's teardown fails to connect
  // (ESP_ERR_HTTP_CONNECT) — a transient back-to-back reconnect issue, not a heap/auth fault.
  // Retry the whole request a few times with a settle delay so the transport can recover. This
  // matters because a dropped PUT means a local delete never reaches the server, so other
  // devices never converge.
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
      esp_http_client_cleanup(client);
      err = ESP_FAIL;
      continue;
    }

    err = esp_http_client_perform(client);
    httpCode = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    endTrace(buf, "BOOKMARKS_PUT", httpCode, err);
    LOG_DBG("KOSync", "Update bookmarks response: %d (err: %d, attempt %d)", httpCode, err, attempt + 1);

    if (err == ESP_OK) break;  // got an HTTP response — no point retrying the transport
  }
  lastHttpCode = httpCode;
  if (err != ESP_OK) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

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
