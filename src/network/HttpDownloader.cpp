#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_wifi.h>

#include <cstdarg>
#include <cstring>
#include <functional>
#include <string>

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 2048;

// X3 HTTPS troubleshooting instrumentation (SdDebugLog "STALL"/"XFER"): a
// per-chunk read taking longer than this is logged with a heap+RSSI snapshot —
// the prior X3 stall investigation found internal-SRAM starvation (WiFi RX
// buffers can't allocate -> dropped frames -> TCP RTO) produced exactly this
// shape of multi-second per-read gap. See SUMMARY.md Part B Appendix.
constexpr uint32_t STALL_LOG_THRESHOLD_MS = 1000;
// Periodic transfer-progress summary cadence. Coarse on purpose: each
// SdDebugLog::log() does two SD opens + a mutex lock, so logging every
// 2KB chunk would itself perturb the transfer being measured.
constexpr size_t XFER_LOG_BYTES = 32 * 1024;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
  // Optional human-readable failure reason, surfaced to the caller (and the UI).
  // Mirrors what's logged to SdDebugLog so a user without a serial cable can see
  // the real cause (status code, OOM, esp_err) on the device screen.
  std::string* detail = nullptr;
};

// snprintf into a stack buffer, then store the reason in *out (if provided).
// Keeps the error path off std::string formatting while still giving the UI text.
void setDetail(std::string* out, const char* fmt, ...) {
  if (!out) return;
  char buf[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  out->assign(buf);
}

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Disable WiFi modem power-save for the duration of a transfer, then restore the
// default. At the default WIFI_PS_MIN_MODEM the radio sleeps between DTIM beacons;
// on a marginal link (observed on the X3, fine on the X4 at the same AP) that
// stalls the TCP window and makes a <1MB feed take minutes, with wild run-to-run
// variance. OtaUpdater already does this around esp_https_ota (OtaUpdater.cpp:141);
// OPDS fetch/download went through the default and paid for it. RAII so every
// early return in runGet restores power-save. Costs extra radio power only while
// a transfer is in flight.
struct NoWifiSleep {
  NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  // Hold WiFi out of modem-sleep for the whole transfer (see NoWifiSleep). Scoped
  // to runGet so it covers the handshake, body read loop, and every early return.
  const NoWifiSleep noWifiSleep;

  // Allocate the read buffer FIRST, before the TLS connection exists. A live
  // mbedtls connection holds ~65KB and fragments the heap; allocating this 2KB
  // buffer afterwards fails on the X3 (only ~6KB, non-contiguous, left). Carving
  // it now — while 70KB+ is free and contiguous — guarantees it.
  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    SdDebugLog::log("HTTP", "OOM: %u byte read buffer, free heap=%u", (unsigned)READ_CHUNK,
                    (unsigned)ESP.getFreeHeap());
    setDetail(sink.detail, "out of memory (heap=%u)", (unsigned)ESP.getFreeHeap());
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    setDetail(sink.detail, "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    SdDebugLog::log("HTTP", "open failed: %s", esp_err_to_name(err));
    setDetail(sink.detail, "connect failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      setDetail(sink.detail, "redirect failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    SdDebugLog::log("HTTP", "unexpected status: %d", status);
    setDetail(sink.detail, "HTTP %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  // Baseline at the start of streaming (post-handshake): pairs with the STALL
  // snapshots taken mid-transfer so a trace shows whether internal-SRAM
  // fragmentation grows over the life of the connection.
  {
    const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
    SdDebugLog::log("CONNECT", "heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d total=%zu url=%s",
                    snap.heapFree, snap.largest8Bit, snap.internalFree, snap.internalLargest, (int)snap.rssi,
                    sink.total, url.c_str());
  }
  const uint32_t transferStartMs = millis();
  uint32_t lastChunkMs = transferStartMs;
  size_t lastXferLogBytes = 0;

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      SdDebugLog::log("HTTP", "read error after %zu bytes, heap=%u", sink.downloaded, (unsigned)ESP.getFreeHeap());
      setDetail(sink.detail, "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received

    // Flag any single read that blocked unusually long. gapMs measures the
    // socket-read latency directly (computed before sink.write touches SD), so
    // it isolates network stalls from SD-write slowness.
    const uint32_t now = millis();
    const uint32_t gapMs = now - lastChunkMs;
    lastChunkMs = now;
    if (gapMs > STALL_LOG_THRESHOLD_MS) {
      const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
      SdDebugLog::log("STALL", "gap=%lums bytes=%zu heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d",
                      (unsigned long)gapMs, sink.downloaded, snap.heapFree, snap.largest8Bit, snap.internalFree,
                      snap.internalLargest, (int)snap.rssi);
    }

    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      SdDebugLog::log("HTTP", "sink write failed after %zu bytes, heap=%u (likely OOM in parser)", sink.downloaded,
                      (unsigned)ESP.getFreeHeap());
      setDetail(sink.detail, "SD write failed after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    // Report progress even when total is unknown (chunked / no Content-Length):
    // callers can show a byte count instead of a percentage bar. total==0 means
    // "size unknown".
    if (sink.progress) sink.progress(sink.downloaded, sink.total);

    if (sink.downloaded - lastXferLogBytes >= XFER_LOG_BYTES) {
      lastXferLogBytes = sink.downloaded;
      const uint32_t elapsedMs = now - transferStartMs;
      const unsigned bytesPerSec = elapsedMs > 0 ? (unsigned)(sink.downloaded * 1000UL / elapsedMs) : 0;
      SdDebugLog::log("XFER", "bytes=%zu elapsed=%lums rate=%uB/s heap=%u", sink.downloaded,
                      (unsigned long)elapsedMs, bytesPerSec, (unsigned)ESP.getFreeHeap());
    }
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);

  {
    const uint32_t totalElapsedMs = millis() - transferStartMs;
    const unsigned bytesPerSec = totalElapsedMs > 0 ? (unsigned)(sink.downloaded * 1000UL / totalElapsedMs) : 0;
    SdDebugLog::log("DONE", "complete=%d bytes=%zu elapsed=%lums rate=%uB/s", (int)complete, sink.downloaded,
                    (unsigned long)totalElapsedMs, bytesPerSec);
  }

  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    SdDebugLog::log("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    setDetail(sink.detail, "incomplete: %zu/%zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             std::string* errorDetail) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    setDetail(errorDetail, "cannot open SD file for write");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.detail = errorDetail;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGet(url, username, password, sink);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    setDetail(errorDetail, "empty response (0 bytes)");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
