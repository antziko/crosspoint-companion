#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <base64.h>
#include <esp_wifi.h>

#include <cstdarg>
#include <functional>
#include <string>

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>
// wolfSSL_Arduino_Serial_Print is defined once in src/network/WolfsslArduinoShim.cpp
// (linking it here too would duplicate the symbol).
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_tls.h>
#include <strings.h>  // strcasecmp (case-insensitive Location header match)

#include <cctype>   // tolower
#include <cstdlib>  // atoi
#include <cstring>
#endif

namespace {
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr int MAX_REDIRECTS = 5;

// X3 HTTPS troubleshooting instrumentation (SdDebugLog "STALL"/"XFER"): a
// per-chunk read taking longer than this is logged with a heap+RSSI snapshot —
// the prior X3 stall investigation found internal-SRAM starvation (WiFi RX
// buffers can't allocate -> dropped frames -> TCP RTO) produced exactly this
// shape of multi-second per-read gap. See SUMMARY.md Part B Appendix.
constexpr uint32_t STALL_LOG_THRESHOLD_MS = 1000;
// Periodic transfer-progress summary cadence. Coarse on purpose: each
// SdDebugLog::log() does two SD opens + a mutex lock, so logging every
// chunk would itself perturb the transfer being measured.
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
// variance. RAII so every early return in runGet restores power-save. Costs extra
// radio power only while a transfer is in flight.
struct NoWifiSleep {
  NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_NONE); }
  ~NoWifiSleep() { esp_wifi_set_ps(WIFI_PS_MIN_MODEM); }
};

#if defined(FREEINK_NET_WOLFSSL)
// ---------------------------------------------------------------------------
// wolfSSL (SecureHttpClient) path — the active TLS stack for app HTTPS.
// ---------------------------------------------------------------------------
// Streams a GET body through sink.write. wolfSSL (TLS 1.3, SP-ECC) fits a
// handshake in ~35-43KB of small allocations, so the mbedTLS arena-fragmentation
// workarounds the esp_http_client path needed (raw-esp-tls redirect resolve,
// per-host CA splitting, deferred read-buffer alloc) are unnecessary here. A
// fresh client per redirect hop keeps state simple; the caller's pinned roots
// verify every hop.
//
// No peer verification (setInsecure) on any hop, matching upstream. The traffic
// stays TLS-encrypted but the server cert is not checked. The caPemOverride/
// caPemRedirect roots (font downloads) are ignored on the wolfSSL path: pinning
// them via setCACert was tried but wolfSSL rejected the handshake with
// ASN_NO_SIGNER_E (-188) — its path builder would not trace the GitHub/CDN chain
// to those roots the way mbedTLS did. The params are retained in the signature
// for the esp_http_client fallback below, which still verifies against them.
HttpDownloader::DownloadError runGet(const std::string& startUrl, const std::string& username,
                                     const std::string& password, Sink& sink, const char* caPemOverride = nullptr,
                                     const char* caPemRedirect = nullptr) {
  // Hold WiFi out of modem-sleep for the whole transfer (see NoWifiSleep).
  const NoWifiSleep noWifiSleep;
  (void)caPemOverride;
  (void)caPemRedirect;

  {
    const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
    SdDebugLog::log("HTTP", "GET start: heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d url=%s", s.heapFree,
                    s.largest8Bit, s.internalFree, s.internalLargest, (int)s.rssi, startUrl.c_str());
  }

  std::string url = startUrl;
  bool retriedConnect = false;
  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      setDetail(sink.detail, "bad URL");
      return HttpDownloader::HTTP_ERROR;
    }
    // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would append
    // a second User-Agent header, which strict servers reject.
    http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (!username.empty() && !password.empty()) http.setBasicAuth(username, password);

    const uint32_t openStartMs = millis();
    uint32_t transferStartMs = openStartMs;
    uint32_t lastChunkMs = openStartMs;
    size_t lastXferLogBytes = 0;
    bool loggedConnect = false;
    // Running split of the transfer: time blocked on the socket vs time inside our own
    // per-chunk work. Totals (not just the >1s outliers) so a transfer that is slow in
    // many small increments is still attributable — the per-chunk thresholds below only
    // ever caught the tail, which is how a 45s feed read as "34.8s of network stalls".
    uint32_t waitTotalMs = 0;
    uint32_t workTotalMs = 0;

    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    const int status = http.GET(
        [&](const uint8_t* data, size_t len) {
          // Header parsing is done by the time the first body chunk arrives; skip
          // any body on a non-200 (a 30x body is drained by the caller loop below).
          if (http.getStatus() != 200) return true;
          if (!loggedConnect) {
            loggedConnect = true;
            transferStartMs = millis();
            lastChunkMs = transferStartMs;
            if (sink.total == 0 && http.hasContentLength()) sink.total = http.getContentLength();
            const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
            SdDebugLog::log("CONNECT",
                            "handshake=%lums heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d total=%zu url=%s",
                            (unsigned long)(transferStartMs - openStartMs), snap.heapFree, snap.largest8Bit,
                            snap.internalFree, snap.internalLargest, (int)snap.rssi, sink.total, url.c_str());
          }

          // Time waiting on the socket since the PREVIOUS callback returned. lastChunkMs
          // is stamped at the END of this callback, not here: stamping it on entry folded
          // our own SD write and the caller's progress repaint into the next "stall", so
          // the two were indistinguishable — a 130KB OPDS feed logged 34.8s of gaps that
          // may have been largely e-ink refreshes, not the link.
          const uint32_t now = millis();
          const uint32_t gapMs = now - lastChunkMs;
          waitTotalMs += gapMs;
          if (gapMs > STALL_LOG_THRESHOLD_MS) {
            const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
            SdDebugLog::log("STALL", "gap=%lums bytes=%zu heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d",
                            (unsigned long)gapMs, sink.downloaded, snap.heapFree, snap.largest8Bit, snap.internalFree,
                            snap.internalLargest, (int)snap.rssi);
          }

          if (!sink.write(data, len)) return false;  // caller abort (e.g. SD write failed)
          sink.downloaded += len;
          // Report progress even when total is unknown (chunked / no Content-Length):
          // callers can show a byte count instead of a percentage bar.
          if (sink.progress) sink.progress(sink.downloaded, sink.total);

          // Our own per-chunk cost: the SD write plus whatever the caller's progress
          // callback did (queueing an e-ink refresh, polling input). Logged separately
          // from STALL so a slow transfer can be attributed to the link or to us
          // without guessing.
          const uint32_t workMs = millis() - now;
          workTotalMs += workMs;
          if (workMs > STALL_LOG_THRESHOLD_MS) {
            SdDebugLog::log("SINK", "work=%lums bytes=%zu len=%zu", (unsigned long)workMs, sink.downloaded, len);
          }
          lastChunkMs = millis();

          if (sink.downloaded - lastXferLogBytes >= XFER_LOG_BYTES) {
            lastXferLogBytes = sink.downloaded;
            const uint32_t elapsedMs = now - transferStartMs;
            const unsigned bytesPerSec = elapsedMs > 0 ? (unsigned)(sink.downloaded * 1000UL / elapsedMs) : 0;
            SdDebugLog::log("XFER", "bytes=%zu elapsed=%lums rate=%uB/s heap=%u", sink.downloaded,
                            (unsigned long)elapsedMs, bytesPerSec, (unsigned)ESP.getFreeHeap());
          }
          return true;
        },
        [&sink]() { return sink.cancelFlag && *sink.cancelFlag; });

    if (http.aborted()) return HttpDownloader::ABORTED;
    if (status < 0) {
      const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      SdDebugLog::log("HTTP", "wolfSSL request failed after %lums heap=%u largest8=%u url=%s",
                      (unsigned long)(millis() - openStartMs), s.heapFree, s.largest8Bit, url.c_str());
      // Retry once when nothing arrived. The ESP32-C3's LWIP stack routinely fails the
      // first connect made after a previous socket closed (TIME_WAIT / sock<0) and then
      // succeeds immediately — the churn KOSync sidesteps with a keep-alive session, but
      // OPDS opens a fresh connection per user action. Observed on X3: this URL failed
      // after 15403ms, then completed its handshake in 588ms on the user's own retry.
      //
      // Guarded on downloaded == 0: past the first body byte the sink already holds
      // partial content, and re-running the GET would append a duplicate copy. The retry
      // spends one hop of the redirect budget rather than tracking a separate counter.
      if (!retriedConnect && sink.downloaded == 0 && hop < MAX_REDIRECTS) {
        retriedConnect = true;
        SdDebugLog::log("HTTP", "connect retry (no bytes yet): %s", url.c_str());
        delay(200);  // let the stack finish tearing the previous socket down
        continue;
      }
      setDetail(sink.detail, "connect/read failed");
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        setDetail(sink.detail, "redirect %d: no Location", status);
        return HttpDownloader::HTTP_ERROR;
      }
      SdDebugLog::log("HTTP", "redirect %d -> %s", status, url.c_str());
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      SdDebugLog::log("HTTP", "unexpected status: %d", status);
      setDetail(sink.detail, "HTTP %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    // A false sink.write return set _callbackAborted; surface it as the SD/parser error.
    if (http.callbackAborted()) {
      SdDebugLog::log("HTTP", "sink write failed after %zu bytes, heap=%u", sink.downloaded,
                      (unsigned)ESP.getFreeHeap());
      setDetail(sink.detail, "SD write failed after %zu bytes", sink.downloaded);
      return HttpDownloader::FILE_ERROR;
    }
    if (!http.responseComplete()) {
      const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      // elapsed + the wait/work split: two X3 book downloads both died here at ~200KB of
      // 1.6MB after 26.7s and 28.2s, which looks like the server closing on its own
      // response deadline while we drained too slowly to finish inside it. If that is
      // right, elapsed stays ~constant across attempts while the byte count tracks
      // whatever throughput we managed.
      SdDebugLog::log("HTTP",
                      "incomplete: got %zu of %zu bytes after %lums (wait=%lums work=%lums) heap=%u largest8=%u",
                      sink.downloaded, sink.total, (unsigned long)(millis() - transferStartMs),
                      (unsigned long)waitTotalMs, (unsigned long)workTotalMs, s.heapFree, s.largest8Bit);
      setDetail(sink.detail, "incomplete: %zu/%zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }
    {
      const uint32_t totalElapsedMs = millis() - transferStartMs;
      const unsigned bytesPerSec = totalElapsedMs > 0 ? (unsigned)(sink.downloaded * 1000UL / totalElapsedMs) : 0;
      // wait= is time blocked on the socket, work= is our own per-chunk cost (SD write
      // plus the caller's progress callback). They should roughly sum to elapsed; a large
      // work= means we are the bottleneck, a large wait= with a small work= means the
      // link or the server is. On X3 a repaint shows up in wait=, not work=, because
      // requestUpdate() only posts to the render task — which then takes the SPI bus the
      // SD card shares, so the NEXT read blocks.
      SdDebugLog::log("DONE", "bytes=%zu elapsed=%lums rate=%uB/s wait=%lums work=%lums", sink.downloaded,
                      (unsigned long)totalElapsedMs, bytesPerSec, (unsigned long)waitTotalMs,
                      (unsigned long)workTotalMs);
    }
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  setDetail(sink.detail, "too many redirects");
  return HttpDownloader::HTTP_ERROR;
}

#else   // !FREEINK_NET_WOLFSSL
// ---------------------------------------------------------------------------
// esp_http_client / mbedTLS path — retained fallback for FREEINK_NET_WOLFSSL=0.
// ---------------------------------------------------------------------------
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
constexpr size_t READ_CHUNK = 2048;

// Capture the redirect Location header into a caller-owned std::string (config.user_data)
// as fetch_headers parses it. Used only by the font per-host-CA path so it can tear the
// (heap-heavy) origin TLS connection down BEFORE building the long CDN redirect URL —
// esp_http_client's own set_redirection/get_url reallocs that string while the origin
// arena is still live, which OOMs on the X4 (~10KB free after the github handshake) and
// asserts in http_utils_append_string. Capturing here sidesteps that entirely.
esp_err_t captureLocationHandler(esp_http_client_event_t* evt) {
  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data && evt->header_key &&
      strcasecmp(evt->header_key, "Location") == 0) {
    static_cast<std::string*>(evt->user_data)->assign(evt->header_value ? evt->header_value : "");
  }
  return ESP_OK;
}

// Resolve ONE redirect hop over raw esp-tls, returning the response status and Location
// without ever buffering the other response headers.
//
// WHY raw esp-tls and not esp_http_client: github's release-download 302 carries a
// 3586-byte content-security-policy header. esp_http_client accumulates every header
// value into a single realloc'd string, and after the TLS handshake the heap's largest
// free block is structurally pinned at ~4.8KB by the mbedtls arena (freeing total heap
// does not enlarge it). Growing a ~3.5KB string there fragments that block and OOM-asserts
// in http_utils_append_string. Here we stream the response through a 512-byte window and a
// byte-state-machine that keeps ONLY the status code and the Location value — every other
// header (the CSP included) is scanned and discarded, so nothing large is ever allocated.
//
// Used only by the font path's origin hop (caPemRedirect set). The CDN target it points to
// sends normal-sized headers, so that hop goes back through esp_http_client.
HttpDownloader::DownloadError resolveRedirectViaTls(const std::string& url, const char* ca, std::string& outLocation,
                                                    int& outStatus, std::string* detail) {
  outLocation.clear();
  outStatus = 0;

  // Parse https://host/path (release URLs are always https, default port 443).
  static const char kScheme[] = "https://";
  if (url.compare(0, sizeof(kScheme) - 1, kScheme) != 0) {
    setDetail(detail, "non-https origin");
    return HttpDownloader::HTTP_ERROR;
  }
  const size_t hostStart = sizeof(kScheme) - 1;
  const size_t slash = url.find('/', hostStart);
  const std::string host = url.substr(hostStart, slash == std::string::npos ? std::string::npos : slash - hostStart);
  const std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);

  esp_tls_cfg_t cfg = {};
  cfg.cacert_buf = reinterpret_cast<const unsigned char*>(ca);
  cfg.cacert_bytes = ca ? static_cast<unsigned int>(strlen(ca) + 1) : 0;  // PEM: include the NUL
  cfg.timeout_ms = HTTP_TIMEOUT_MS;
  // No alpn_protos -> no "h2" offer -> github replies HTTP/1.1 (plain-text headers we can
  // scan). Offering h2 would give HPACK-binary headers.

  {
    const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
    SdDebugLog::log("HTTP", "tls-resolve pre-open: heap=%u largest8=%u host=%s", s.heapFree, s.largest8Bit,
                    host.c_str());
  }

  esp_tls_t* tls = esp_tls_init();
  if (!tls) {
    setDetail(detail, "esp_tls_init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  const uint32_t startMs = millis();
  const int conn = esp_tls_conn_new_sync(host.c_str(), static_cast<int>(host.size()), 443, &cfg, tls);
  if (conn != 1) {
    const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
    LOG_ERR("HTTP", "tls-resolve handshake failed: %d", conn);
    SdDebugLog::log("HTTP", "tls-resolve open failed ret=%d after %lums heap=%u largest8=%u", conn,
                    (unsigned long)(millis() - startMs), s.heapFree, s.largest8Bit);
    setDetail(detail, "tls connect failed (%d)", conn);
    esp_tls_conn_destroy(tls);
    return HttpDownloader::HTTP_ERROR;
  }
  {
    const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
    SdDebugLog::log("HTTP", "tls-resolve post-open: handshake=%lums heap=%u largest8=%u",
                    (unsigned long)(millis() - startMs), s.heapFree, s.largest8Bit);
  }

  std::string req;
  req.reserve(path.size() + host.size() + 96);
  req += "GET ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += host;
  req += "\r\nUser-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION "\r\nAccept: */*\r\nConnection: close\r\n\r\n";

  for (size_t written = 0; written < req.size();) {
    const ssize_t w = esp_tls_conn_write(tls, req.data() + written, req.size() - written);
    if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
    if (w < 0) {
      LOG_ERR("HTTP", "tls-resolve write failed: %d", (int)w);
      setDetail(detail, "tls write failed (%d)", (int)w);
      esp_tls_conn_destroy(tls);
      return HttpDownloader::HTTP_ERROR;
    }
    written += static_cast<size_t>(w);
  }

  // Byte state machine over the response. Keeps the status code and the Location value;
  // every other header line (incl. the 3.5KB CSP) is matched-and-discarded char by char,
  // so the only growing allocation is the ~900-byte Location string.
  enum class St { Status, HdrStart, MatchLoc, SkipEol, LocVal, Done };
  St st = St::Status;
  char statusBuf[40];
  int statusLen = 0;
  static const char kLoc[] = "location:";  // lowercase incl. colon
  int locMatch = 0;

  char rbuf[512];
  while (st != St::Done) {
    const ssize_t n = esp_tls_conn_read(tls, rbuf, sizeof(rbuf));
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
    if (n < 0) {
      LOG_ERR("HTTP", "tls-resolve read failed: %d", (int)n);
      setDetail(detail, "tls read failed (%d)", (int)n);
      esp_tls_conn_destroy(tls);
      return HttpDownloader::HTTP_ERROR;
    }
    if (n == 0) break;  // peer closed before headers ended
    for (int i = 0; i < n && st != St::Done; ++i) {
      const char c = rbuf[i];
      switch (st) {
        case St::Status:
          if (c == '\n') {
            st = St::HdrStart;
            locMatch = 0;
          } else if (c != '\r' && statusLen < static_cast<int>(sizeof(statusBuf) - 1)) {
            statusBuf[statusLen++] = c;
          }
          break;
        case St::HdrStart:
          if (c == '\n') {  // blank line (\r already ignored) -> headers complete
            st = St::Done;
          } else if (c == '\r') {
            // ignore; wait for the \n
          } else if (static_cast<char>(tolower(c)) == kLoc[locMatch]) {
            if (kLoc[++locMatch] == '\0') st = St::LocVal;  // matched "location:"
          } else {
            st = St::SkipEol;  // some other header (e.g. CSP) -> discard to EOL
          }
          break;
        case St::SkipEol:
          if (c == '\n') {
            st = St::HdrStart;
            locMatch = 0;
          }
          break;
        case St::LocVal:
          if (c == '\n') {
            st = St::HdrStart;
            locMatch = 0;
          } else if (c != '\r' && !(outLocation.empty() && c == ' ')) {
            outLocation.push_back(c);
          }
          break;
        case St::Done:
          break;
      }
    }
  }
  esp_tls_conn_destroy(tls);

  statusBuf[statusLen] = '\0';
  const char* sp = strchr(statusBuf, ' ');  // "HTTP/1.1 302 Found" -> code after first space
  outStatus = sp ? atoi(sp + 1) : 0;
  return HttpDownloader::OK;
}

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink, const char* caPemOverride = nullptr,
                                     const char* caPemRedirect = nullptr) {
  // Hold WiFi out of modem-sleep for the whole transfer (see NoWifiSleep). Scoped
  // to runGet so it covers the handshake, body read loop, and every early return.
  const NoWifiSleep noWifiSleep;

  // Entry snapshot: the cleanest heap state for this request (before the read
  // buffer, the esp_http_client struct, or the TLS arena exist). The delta from
  // here to the post-open snapshot below is the per-request + handshake cost.
  {
    const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
    SdDebugLog::log("HTTP", "GET start: heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d url=%s", s.heapFree,
                    s.largest8Bit, s.internalFree, s.internalLargest, (int)s.rssi, url.c_str());
  }

  // Allocate the read buffer FIRST, before the TLS connection exists. A live
  // mbedtls connection holds ~65KB and fragments the heap; allocating this 2KB
  // buffer afterwards fails on the X3 (only ~6KB, non-contiguous, left). Carving
  // it now — while 70KB+ is free and contiguous — guarantees it.
  //
  // EXCEPTION: the font path (caPemOverride) handshakes to github's all-ECDSA chain,
  // whose verify-time MPI scratch on the X4 exhausts the heap to <1KB. There this 2KB
  // sitting idle through the handshake is the difference between the cert verify
  // allocating or OOMing (-0x2700), so defer it to just before the body read instead.
  // X3 fonts already can't clear this wall, so deferring costs them nothing.
  std::unique_ptr<char[]> buf;
  if (!caPemOverride) {
    buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
    if (!buf) {
      LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
      SdDebugLog::log("HTTP", "OOM: %u byte read buffer, free heap=%u", (unsigned)READ_CHUNK,
                      (unsigned)ESP.getFreeHeap());
      setDetail(sink.detail, "out of memory (heap=%u)", (unsigned)ESP.getFreeHeap());
      return HttpDownloader::HTTP_ERROR;
    }
  }

  // DIAGNOSTIC (font CA path only): force the esp-tls / mbedtls verify chatter to
  // VERBOSE so a failed github handshake prints the *reason* (e.g. "Failed to verify
  // peer certificate! ... not correctly signed by the trusted CA" for a real CA
  // mismatch, vs "mbedtls_mpi ... alloc" for OOM-in-verify). Remove once the font
  // wall is settled. OPDS/KOSync (caPemOverride==null) never set this.
  if (caPemOverride) {
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("mbedtls", ESP_LOG_VERBOSE);
  }

  // Build + open one HTTP(S) hop to `hopUrl`, verifying against `ca` (a pinned root
  // PEM) or the full bundle when `ca` is null. On success, returns OK with `outClient`
  // open and `outStatus`/`outLen` populated; on any failure it cleans up its own client
  // and returns the error (outClient left null). Used for the initial request and, on
  // the font per-host-CA path, for each redirect hop with a different pinned root.
  auto openHop = [&](const std::string& hopUrl, const char* ca, int rxSize, int txSize,
                     esp_http_client_handle_t& outClient, int& outStatus, int64_t& outLen,
                     std::string* outLocation) -> HttpDownloader::DownloadError {
    outClient = nullptr;
    esp_http_client_config_t config = {};
    config.url = hopUrl.c_str();
    // Capture the redirect Location ourselves (see captureLocationHandler) instead of
    // esp_http_client's set_redirection, which reallocs the URL while the TLS arena is
    // live and OOMs. Used by the font CDN hops; null for OPDS/KOSync.
    if (outLocation) {
      outLocation->clear();
      config.event_handler = captureLocationHandler;
      config.user_data = outLocation;
    }
    // buffer_size (HTTP RX) / buffer_size_tx are allocated at init and held through the
    // whole handshake, so the caller tunes them per hop. (The font ORIGIN hop — github,
    // with its 3.5KB CSP header that esp_http_client can't parse under the arena-fragmented
    // heap — does NOT use this lambda; it goes through resolveRedirectViaTls. This lambda
    // serves the font CDN hops, which send small headers, plus the unchanged OPDS path.)
    config.buffer_size = rxSize;
    config.buffer_size_tx = txSize;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    // Verify HTTPS against the pinned root (ca) or the bundled CA roots. This build has
    // esp-tls CONFIG_ESP_TLS_INSECURE off, so an unverified handshake can't be set up;
    // the model is public servers over verified https and local servers over plain http
    // (esp_http_client picks the transport from the URL scheme, so http:// needs no cert
    // config). caPemOverride pins specific roots instead of the bundle for callers whose
    // chain the prebuilt bundle mis-verifies (FontDownloadActivity — see FontDownloadCA.h).
    if (ca) {
      config.cert_pem = ca;
    } else {
      config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    config.keep_alive_enable = true;

    esp_http_client_handle_t c = esp_http_client_init(&config);
    if (!c) {
      LOG_ERR("HTTP", "client init failed");
      setDetail(sink.detail, "client init failed");
      return HttpDownloader::HTTP_ERROR;
    }
    esp_http_client_set_header(c, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (!username.empty() && !password.empty()) {
      // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
      const std::string credentials = username + ":" + password;
      const String header = "Basic " + base64::encode(credentials.c_str());
      esp_http_client_set_header(c, "Authorization", header.c_str());
    }

    // Snapshot immediately before the (blocking) connect+TLS handshake, then again after,
    // with the handshake duration. The before/after heap delta is the mbedTLS arena cost —
    // the bulk of the HTTPS pressure on the X3/X4 — and a failed open on https:// is almost
    // always that arena OOMing, surfaced as ESP_ERR_HTTP_CONNECT.
    {
      const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
      SdDebugLog::log("HTTP", "pre-open (handshake): heap=%u largest8=%u intFree=%u intLargest=%u", s.heapFree,
                      s.largest8Bit, s.internalFree, s.internalLargest);
    }
    const uint32_t openStartMs = millis();
    const esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
      const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
      LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
      SdDebugLog::log("HTTP", "open failed: %s after %lums, heap=%u largest8=%u intFree=%u intLargest=%u",
                      esp_err_to_name(err), (unsigned long)(millis() - openStartMs), s.heapFree, s.largest8Bit,
                      s.internalFree, s.internalLargest);
      setDetail(sink.detail, "connect failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(c);
      return HttpDownloader::HTTP_ERROR;
    }
    {
      const SdDebugLog::NetSnapshot s = SdDebugLog::captureNetSnapshot();
      SdDebugLog::log("HTTP", "post-open: handshake=%lums heap=%u largest8=%u intFree=%u intLargest=%u",
                      (unsigned long)(millis() - openStartMs), s.heapFree, s.largest8Bit, s.internalFree,
                      s.internalLargest);
    }
    outLen = esp_http_client_fetch_headers(c);
    outStatus = esp_http_client_get_status_code(c);
    outClient = c;
    return HttpDownloader::OK;
  };

  // open()/read() does not auto-follow redirects (only perform() does), so step 30x
  // responses manually.
  esp_http_client_handle_t client = nullptr;
  int status = 0;
  int64_t contentLength = 0;

  if (caPemRedirect) {
    // FONT PATH. The github origin returns a 302 with a 3586-byte content-security-policy
    // header that esp_http_client cannot parse under the arena-fragmented heap (see
    // resolveRedirectViaTls). Resolve that hop over raw esp-tls, keeping ONLY the Location,
    // then fetch the CDN target with the normal client — its headers are small. Per-host CA:
    // caPemOverride pins the origin (github) root, caPemRedirect pins the CDN (release-assets)
    // root, so each handshake's mbedTLS arena holds a single root.
    std::string target;
    {
      std::string loc;
      int rstatus = 0;
      const HttpDownloader::DownloadError e = resolveRedirectViaTls(url, caPemOverride, loc, rstatus, sink.detail);
      if (e != HttpDownloader::OK) return e;
      SdDebugLog::log("HTTP", "tls-resolve: status=%d loc=%zu", rstatus, loc.size());
      if (!isRedirect(rstatus) || loc.empty()) {
        LOG_ERR("HTTP", "origin resolve failed: status=%d loc=%zu", rstatus, loc.size());
        setDetail(sink.detail, "redirect resolve failed (%d)", rstatus);
        return HttpDownloader::HTTP_ERROR;
      }
      target = loc;
    }
    // CDN hop buffers. The release-assets chain is all-RSA (leaf <- YR2 <- Root YR <- ISRG
    // Root X1, our anchor), and the RSA-4096 verify at the top rides the heap ceiling
    // (min-free hit 424). Keep these tight: RX 1KB holds the CDN's modest response headers
    // (no github-style CSP here), TX 1.25KB fits the ~870-byte signed-URL GET line plus its
    // Host/UA headers. Trimming from 4KB to ~2.25KB frees the contiguous heap the RSA verify
    // needs. CONNECT cost only; the deferred body read buffer is still not allocated here.
    constexpr int kCdnRx = 1024;
    constexpr int kCdnTx = 1280;
    std::string location;
    const HttpDownloader::DownloadError e =
        openHop(target, caPemRedirect, kCdnRx, kCdnTx, client, status, contentLength, &location);
    if (e != HttpDownloader::OK) return e;
    for (int hop = 0; isRedirect(status) && hop < 4; ++hop) {
      if (location.empty()) {
        LOG_ERR("HTTP", "CDN redirect %d without Location", status);
        setDetail(sink.detail, "redirect %d: no Location", status);
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      const std::string next = location;
      esp_http_client_cleanup(client);
      client = nullptr;
      const HttpDownloader::DownloadError e2 =
          openHop(next, caPemRedirect, kCdnRx, kCdnTx, client, status, contentLength, &location);
      if (e2 != HttpDownloader::OK) return e2;
    }
  } else {
    // Default path (OPDS/KOSync/OTA and bundle callers): single client, follow redirects on
    // the SAME connection/cert, exactly as before. Unchanged to avoid any TLS regression.
    const HttpDownloader::DownloadError e =
        openHop(url, caPemOverride, caPemOverride ? 2048 : HTTP_RX_BUF, caPemOverride ? 512 : HTTP_TX_BUF, client,
                status, contentLength, nullptr);
    if (e != HttpDownloader::OK) return e;
    for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
      if (esp_http_client_set_redirection(client) != ESP_OK) break;
      const esp_err_t err = esp_http_client_open(client, 0);
      if (err != ESP_OK) {
        LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
        setDetail(sink.detail, "redirect failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      contentLength = esp_http_client_fetch_headers(client);
      status = esp_http_client_get_status_code(client);
    }
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
    SdDebugLog::log("CONNECT", "heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d total=%zu url=%s", snap.heapFree,
                    snap.largest8Bit, snap.internalFree, snap.internalLargest, (int)snap.rssi, sink.total, url.c_str());
  }
  // Deferred read-buffer alloc for the font path (see the early-alloc comment). The
  // handshake is done and the TLS record buffers have settled, so the 2KB is available
  // again now without having starved the cert verify.
  if (!buf) {
    buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
    if (!buf) {
      LOG_ERR("HTTP", "OOM: %u byte read buffer (post-handshake)", (unsigned)READ_CHUNK);
      SdDebugLog::log("HTTP", "OOM: %u byte read buffer post-handshake, free heap=%u", (unsigned)READ_CHUNK,
                      (unsigned)ESP.getFreeHeap());
      setDetail(sink.detail, "out of memory (heap=%u)", (unsigned)ESP.getFreeHeap());
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
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
      // Surface the real cause. esp_http_client_read collapses every failure to
      // <0, so pull the socket errno, the last esp-tls/mbedtls codes, AND the
      // contiguous-heap snapshot. X3 and X4 run the same 8192-IN lib, so a record
      // > IN would fail both — yet only X3 fails, which points at heap headroom,
      // not record size. Distinguish: -0x7f00 (ALLOC_FAILED) or ENOMEM with a low
      // largest8 == out-of-contiguous-heap; -0x7200 (INVALID_RECORD) == record
      // overflow; a bare errno == socket timeout/reset.
      const int sockErrno = esp_http_client_get_errno(client);
      int tlsCode = 0, tlsFlags = 0;
      esp_http_client_get_and_clear_last_tls_error(client, &tlsCode, &tlsFlags);
      const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
      LOG_ERR("HTTP", "read error after %zu bytes (ret=%d errno=%d mbedtls=-0x%04x flags=0x%x)", sink.downloaded, read,
              sockErrno, -tlsCode, tlsFlags);
      SdDebugLog::log("HTTP",
                      "read error after %zu bytes, ret=%d errno=%d mbedtls=-0x%04x flags=0x%x heap=%u largest8=%u "
                      "intFree=%u intLargest=%u",
                      sink.downloaded, read, sockErrno, -tlsCode, tlsFlags, snap.heapFree, snap.largest8Bit,
                      snap.internalFree, snap.internalLargest);
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
      SdDebugLog::log("XFER", "bytes=%zu elapsed=%lums rate=%uB/s heap=%u", sink.downloaded, (unsigned long)elapsedMs,
                      bytesPerSec, (unsigned)ESP.getFreeHeap());
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
#endif  // FREEINK_NET_WOLFSSL
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
                                                             std::string* errorDetail, const char* caPemOverride,
                                                             const char* caPemRedirect) {
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

  const DownloadError result = runGet(url, username, password, sink, caPemOverride, caPemRedirect);
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
