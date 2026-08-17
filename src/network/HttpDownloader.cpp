#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <base64.h>
#include <esp_wifi.h>
#include <strings.h>  // strncasecmp (case-insensitive scheme match)

#include <cstdarg>
#include <functional>
#include <memory>
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
// wolfSSL's MEMORY_E, spelled out rather than including wolfSSL's error headers here just
// to name one code. Reported by SecureHttpClient::lastTlsError() when an incoming record
// could not be buffered.
constexpr int WOLFSSL_MEMORY_E = -125;
// Range-resume budget for a body that dies mid-stream. A flat hop count is really a
// cap on FILE SIZE, and it was set from an optimistic per-hop figure: at the ~215KB
// per connection the X3 was once measured at, 16 hops covered ~3.5MB, but a heap
// under more pressure gets far less. Measured on a 7714005-byte OPDS book download
// (opds_debug.txt): 16 hops delivering 53046..206093 bytes each, EVERY ONE making
// healthy forward progress, ran the budget out at 1364258 bytes (18%) and failed a
// download that was converging perfectly well.
//
// So budget on CONVERGENCE, not on hop count: a hop that advances less than
// MIN_RESUME_HOP_BYTES is a stall, and only consecutive stalls end the loop. The
// absolute ceiling stays as a backstop against a server that dribbles just over the
// stall threshold forever; at the measured 53KB/hop floor it covers ~6.8MB, and at
// the 206KB best case ~26MB. Every hop still costs one TLS handshake (~1.3s measured)
// and must make strict forward progress, and the caller's cancel flag is polled per
// chunk, so a user can always abort.
constexpr int MAX_RESUME_ATTEMPTS = 128;
constexpr size_t MIN_RESUME_HOP_BYTES = 16 * 1024;
constexpr int MAX_RESUME_STALLS = 4;
// Full restarts allowed when a server answers 200 to a Range request (i.e. it does
// not support resuming at all). Two, because a restart is only worth attempting while
// a fresh connection still has a real chance: the same 130676-byte feed completed
// outright on 2 of 5 attempts in one session, so a couple of retries convert the
// user's manual "try again, try again" into one operation, while more than that would
// just burn radio time on a link that clearly cannot hold the transfer.
constexpr int MAX_RANGE_RESTARTS = 2;
// Smallest https body worth leasing the TLS record slab for (see the lease site in
// runGet). The failure it guards against is the ~200KB-per-connection record wall; the
// largest feed either OPDS server here serves is 130668 bytes and has never needed it,
// while book downloads are megabytes. 128KB sits just under that measured feed size, so
// feeds keep their heap and downloads keep their block.
constexpr size_t SLAB_MIN_BODY_BYTES = 128 * 1024;

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
  // Optional: throw away everything written so far and reopen the destination empty,
  // so the transfer can start over at byte 0. Only a destination that can be truncated
  // can offer this (downloadToFile); when it is null a server that refuses Range is a
  // hard failure, which is the previous behaviour. Raw function pointer + context
  // rather than a second std::function signature — see the KoStreamSink precedent in
  // KOReaderSyncClient.cpp and the template-bloat rule in CLAUDE.md.
  void* rewindCtx = nullptr;
  bool (*rewind)(void* ctx) = nullptr;
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

// Scheme test for the TLS record-slab lease. Case-insensitive per RFC 3986 3.1, matching
// SecureHttpClient::parseUrl, so a "HTTPS://" Location header is not mistaken for plain HTTP.
bool isHttpsUrl(const std::string& url) { return url.size() >= 8 && strncasecmp(url.c_str(), "https://", 8) == 0; }

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
  int redirects = 0;
  // Byte offset a resumed hop asks the server to continue from (0 = fresh request).
  // See the truncation handler below for why a mid-stream drop is resumed rather
  // than failed.
  size_t resumeOffset = 0;
  int resumes = 0;
  // Hops that advanced less than MIN_RESUME_HOP_BYTES, in a row. Reset by any hop that
  // makes real progress, so a converging transfer is never cut off by its own length.
  int stalledResumes = 0;
  // Full restarts spent because the server answered 200 to a Range request.
  int rangeRestarts = 0;
  // wolfSSL error that ended the PREVIOUS hop, 0 for a hop that ended cleanly. Read by the
  // range-restart decision below, which must know whether the last failure was one a fresh
  // connection could plausibly avoid.
  int lastHopTlsErr = 0;

  // Back wolfSSL's per-record receive buffer with one block bought here, at the healthiest
  // heap state this request will ever see, and held across every redirect and resume hop.
  //
  // Without it wolfSSL re-allocates ~16.4KB contiguous for each incoming record (it frees
  // the buffer after every record it delivers — see TlsRecordSlab), which on the X3's
  // post-WiFi heap fails partway through and kills the body with MEMORY_E (tlsErr=-125).
  // Captures show that ending every HTTPS transfer at ~200-215KB: servers that honour
  // Range hide it as a long string of resumed hops, and a server that refuses Range cannot
  // finish at all because each restart re-runs the same 205KB and dies again. The same
  // files over plain HTTP, same sink, complete at 1.7MB with no retries.
  //
  // Scoped to this transfer: outside it wolfSSL allocates exactly as before, so KOSync and
  // feed fetches are untouched. If the block cannot be bought the lease is inactive and
  // behaviour is today's, so this can help and cannot regress.
  //
  // ONLY for an https hop. SecureHttpClient runs plain http over its WiFiClient transport
  // (ensureConnected()) and never enters wolfSSL, so on an http:// URL this block is 17KB
  // of dead weight held for the whole transfer — and on the X3 that is the difference
  // between a feed that reads and one that does not. Measured against one plain-HTTP host
  // in a single session (opds_debug.txt): every request that took the lease lost ~18KB at
  // GET start (38872 -> 20528 free) and entered its body with 6012 free / 2420 largest,
  // and the 37977-byte feed died mid-body at 11426 and again at 5682 bytes; the requests
  // where the malloc happened to fail (logged active=0, so no lease) streamed a
  // 1676098-byte download to completion at 233KB/s. Every completed plain-HTTP transfer in
  // that capture also logged slabHit=0 slabMiss=0 — the block was never handed out once,
  // because wolfSSL was not in the path to ask for it.
  //
  // Held indirectly, and bought at the FIRST BODY BYTE rather than before the request —
  // see the lease site in the body callback for why the handshake must not pay for it.
  std::unique_ptr<freeink::TlsRecordSlab> recordSlab;

  for (;;) {
    // Re-evaluated per hop: a redirect can cross schemes in either direction.
    const bool secureHop = isHttpsUrl(url);

    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      setDetail(sink.detail, "bad URL");
      return HttpDownloader::HTTP_ERROR;
    }
    if (resumeOffset > 0) {
      http.addHeader("Range", "bytes=" + std::to_string(resumeOffset) + "-");
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
          // any body on a non-200/206 (a 30x body is drained by the caller loop below).
          if (http.getStatus() != 200 && http.getStatus() != 206) return true;
          // A resumed hop that answers 200 ignored our Range header and is restarting the
          // resource from byte 0. Appending it would duplicate what the sink already holds
          // (measured on X3 before this check existed: a 130676-byte feed grew to 217792
          // bytes over 15s before the post-GET status check noticed), so it must never be
          // appended.
          //
          // But the server is, right now, sending exactly the bytes we want from exactly
          // the offset we can use — so take them. Empty the destination and treat this
          // response as a fresh download instead of throwing the connection away: no extra
          // handshake, no wasted round trip, and it turns a dead end into the retry the
          // user was performing by hand. books.yapaa.org's generated feed endpoints ignore
          // Range, and the same feed completed outright on 2 of 5 attempts in one session,
          // so a fresh full body has a real chance where a resume has none.
          //
          // sink.total is deliberately NOT reset: it was learned from hop 1's
          // Content-Length and is the full resource size, which is what the progress bar
          // and the completeness check below both want. resumeOffset going to 0 also makes
          // the `sink.total == 0 && resumeOffset == 0` guard below inert, as intended.
          //
          // Without a rewind (a sink that cannot be truncated) or out of restart budget,
          // fall back to refusing the first byte; the status check below the GET then
          // reports "resume unsupported" exactly as before, just far earlier.
          if (resumeOffset > 0 && http.getStatus() == 200) {
            // ...but only when the previous hop died of something a fresh connection might
            // not repeat. MEMORY_E is not that. The record wall sits at a fixed heap size,
            // so a restart replays the identical transfer into the identical wall: measured
            // on an X3, three restarts of one article.epub died at 204762, 217647 and
            // 215408 bytes — ~6s of radio and 640KB of traffic spent to re-prove what the
            // first hop already established. Refuse the byte instead and let the
            // "resume unsupported" branch below report it truthfully, at once.
            if (lastHopTlsErr == WOLFSSL_MEMORY_E) {
              SdDebugLog::log("HTTP", "range ignored at %zu after MEMORY_E -> restart would hit the same wall",
                              resumeOffset);
              return false;
            }
            if (!sink.rewind || rangeRestarts >= MAX_RANGE_RESTARTS || !sink.rewind(sink.rewindCtx)) return false;
            ++rangeRestarts;
            SdDebugLog::log("HTTP", "range ignored at %zu -> restarting from 0 (%d/%d)", resumeOffset, rangeRestarts,
                            MAX_RANGE_RESTARTS);
            resumeOffset = 0;
            sink.downloaded = 0;
            lastXferLogBytes = 0;
            stalledResumes = 0;
          }
          if (!loggedConnect) {
            loggedConnect = true;
            transferStartMs = millis();
            lastChunkMs = transferStartMs;
            // A 206's Content-Length is the length of the RANGE, not of the resource,
            // so never let it overwrite the full size learned on the first hop. The
            // sink.total == 0 guard already covers this (a resume only happens once
            // total is known); resumeOffset makes the intent explicit.
            if (sink.total == 0 && resumeOffset == 0 && http.hasContentLength()) sink.total = http.getContentLength();
            const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
            SdDebugLog::log(
                "CONNECT",
                "handshake=%lums resumed=%d heap=%u largest8=%u intFree=%u intLargest=%u rssi=%d total=%zu url=%s",
                (unsigned long)(transferStartMs - openStartMs), http.tlsSessionResumed() ? 1 : 0, snap.heapFree,
                snap.largest8Bit, snap.internalFree, snap.internalLargest, (int)snap.rssi, sink.total, url.c_str());

            // Lease the record slab HERE, at the first body byte — never before the request.
            //
            // The TLS handshake, not the body, is the peak allocation of a session: wolfSSL
            // TLS 1.3 with SP-ECC needs ~35-43KB of small blocks for it. Buying 17408 of
            // them up front left the handshake ~22KB and killed every https fetch in the
            // 08-16 capture before it ever connected — GET start 41124 free, 22612 after
            // the lease, then "wolfSSL request failed" with no CONNECT line at all; four
            // attempts across two hosts, all identical. The control is KOSync: same wolfSSL
            // stack, same https, ~43KB free, no lease anywhere in its path, 200 every time.
            // By the time a body byte arrives the handshake scratch is freed and the record
            // buffer is the only thing left that wants a big contiguous block, which is the
            // one this was ever meant to serve.
            //
            // Leased ONLY for a body whose Content-Length says it can reach the
            // ~200KB-per-connection record wall documented at the truncation handler below.
            // An unknown length does NOT lease, and that is the whole point: read.yapaa.org
            // frames every response — feed and download alike — without a Content-Length, so
            // treating "unknown" as "might be big" leased the block for a 15827-byte feed,
            // took heap to 11620 / largest 4340, and killed the fetch (tlsErr=-397 after
            // 9178ms) on a request that would otherwise have finished in 143ms. Unknown
            // length is not evidence of a large body; it is the absence of evidence.
            //
            // Expect active=0 here on the X3 and do not "fix" that by moving the lease
            // earlier: post-handshake the largest free block is ~14324 while this block is
            // 17408, so it cannot be bought at this point — and it cannot be bought before
            // the handshake either, which is what 14c proved. A failed lease is inert, so
            // this stays correct where the contiguous heap does exist.
            if (!recordSlab && secureHop && sink.total >= SLAB_MIN_BODY_BYTES) {
              recordSlab = makeUniqueNoThrow<freeink::TlsRecordSlab>();
              const SdDebugLog::NetSnapshot after = SdDebugLog::captureNetSnapshot();
              SdDebugLog::log("HTTP", "tls slab: active=%d total=%zu size=%u heap=%u largest8=%u",
                              (recordSlab && recordSlab->active()) ? 1 : 0, sink.total,
                              (unsigned)freeink::TlsRecordSlab::size(), after.heapFree, after.largest8Bit);
            }
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
            // largest8 is the number that decides whether an https body survives, and it was
            // the one this line did not print. wolfSSL sizes its receive buffer to each
            // incoming record and servers ramp record size as a connection warms, so the
            // body dies the first time a record needs more contiguous heap than exists.
            // Free heap alone cannot show that coming: at the 08-16 wall free was 21700 while
            // largest was 14324 against a ~16717 requirement — a 2.4KB deficit invisible in
            // the free figure. Printing it per 32KB shows the trajectory instead of the
            // post-mortem, which is what says whether that gap is closable at all.
            const SdDebugLog::NetSnapshot xs = SdDebugLog::captureNetSnapshot();
            SdDebugLog::log("XFER", "bytes=%zu elapsed=%lums rate=%uB/s heap=%u largest8=%u", sink.downloaded,
                            (unsigned long)elapsedMs, bytesPerSec, xs.heapFree, xs.largest8Bit);
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
      // "This hop delivered nothing" — the condition both recovery paths below need, and
      // NOT the same as "the transfer holds no bytes". resumeOffset and sink.downloaded
      // are set equal at the top of every hop (by both resume branches below and by the
      // range-restart rewind) and only the body callback moves them apart. Re-issuing
      // such a hop is idempotent: it carries `Range: bytes=<resumeOffset>-` and appends
      // exactly what the dead hop would have, so there is no duplicate to fear. On hop 1
      // resumeOffset is 0 and this is the original `sink.downloaded == 0` unchanged.
      const bool hopDeliveredNothing = sink.downloaded == resumeOffset;

      // Retry once, immediately. The ESP32-C3's LWIP stack routinely fails the first
      // connect made after a previous socket closed (TIME_WAIT / sock<0) and then
      // succeeds immediately — the churn KOSync sidesteps with a keep-alive session, but
      // OPDS opens a fresh connection per hop. Observed on X3: this URL failed after
      // 15403ms, then completed its handshake in 588ms on the user's own retry.
      if (!retriedConnect && hopDeliveredNothing) {
        retriedConnect = true;
        SdDebugLog::log("HTTP", "connect retry (no bytes this hop): %s", url.c_str());
        delay(200);  // let the stack finish tearing the previous socket down
        continue;
      }

      // A dead connect on a RESUME hop is a zero-byte hop, not the end of the download.
      //
      // Measured on X3 (opds_debug.txt): two runs at a 24139108-byte book carried 3452556
      // and 2138234 bytes over 21 and 11 hops of clean forward progress, then both ended
      // identically — the server closed mid-hop (tlsErr=-397 SOCKET_PEER_CLOSED_E) after
      // a >26s stall, the reconnect failed in 4ms, and the whole partial download was
      // discarded with over 100 resume attempts unspent. Neither memory nor the link was
      // the problem: that 4ms failure was measured at 37248 free / 19444 largest, and a
      // fresh connection to the same host 17s later handshook in 749ms and pulled a
      // 130676-byte feed clean. A single 200ms retry is not always enough for whatever
      // the far end is doing after ~100s of range requests, so give the hop the same
      // convergence budget a truncated one gets.
      //
      // Sharing stalledResumes with the truncation path keeps the total bounded and lets
      // any healthy hop reset it, so a long download that hiccups once every twenty hops
      // never accumulates toward the ceiling. resumeOffset > 0 keeps this off the first
      // hop: there is no partial body to protect there, and a server refusing the very
      // first connect should fail fast rather than after four backoffs.
      if (resumeOffset > 0 && hopDeliveredNothing && stalledResumes < MAX_RESUME_STALLS &&
          resumes < MAX_RESUME_ATTEMPTS && !(sink.cancelFlag && *sink.cancelFlag)) {
        ++stalledResumes;
        ++resumes;
        retriedConnect = false;  // the next hop gets its own one-shot immediate retry
        // lastHopTlsErr is deliberately NOT touched: it records the error of the last hop
        // that actually moved bytes, which is what the range-restart decision reads. A
        // connect that never opened says nothing about record sizes.
        const uint32_t backoffMs = 500u * static_cast<uint32_t>(stalledResumes);
        SdDebugLog::log("HTTP", "connect failed on resume hop at %zu, backing off %lums (stall %d/%d, attempt %d/%d)",
                        resumeOffset, (unsigned long)backoffMs, stalledResumes, MAX_RESUME_STALLS, resumes,
                        MAX_RESUME_ATTEMPTS);
        // Sliced, because nothing polls input during a backoff — the caller's progress
        // callback only runs on body chunks — and at the top of the range that would be a
        // 2s window with a dead Cancel button.
        for (uint32_t slept = 0; slept < backoffMs && !(sink.cancelFlag && *sink.cancelFlag); slept += 100) {
          delay(100);
        }
        if (sink.cancelFlag && *sink.cancelFlag) return HttpDownloader::ABORTED;
        continue;
      }
      // Name the failure the user actually hit. "connect/read failed" is right for a hop
      // that never opened, but says nothing after a resumed transfer has carried
      // megabytes and then lost the far end — which is the case this branch now reaches
      // only once the backoff above is spent.
      if (resumeOffset > 0) {
        setDetail(sink.detail, "connection lost at %zu bytes; server stopped responding", sink.downloaded);
      } else {
        setDetail(sink.detail, "connect/read failed");
      }
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      if (++redirects > MAX_REDIRECTS) break;
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        setDetail(sink.detail, "redirect %d: no Location", status);
        return HttpDownloader::HTTP_ERROR;
      }
      SdDebugLog::log("HTTP", "redirect %d -> %s", status, url.c_str());
      continue;
    }
    // A resumed hop MUST answer 206. A 200 means the server ignored the Range header
    // and is restarting the body from byte 0 — appending that to what we already hold
    // would silently corrupt the file, so stop instead. The body callback already
    // refused the first byte for this reason, so sink.downloaded below is still the
    // honest pre-hop count; this branch also covers a 200 that carried no body at all.
    if (resumeOffset > 0 && status == 200) {
      SdDebugLog::log("HTTP", "resume unsupported (200 for Range at %zu bytes, tlsErr=%d)", resumeOffset,
                      lastHopTlsErr);
      // Name the real cause. "incomplete: 215408/0 bytes" sent the user looking at their
      // network, which was moving at ~120KB/s when the body died; the actual pairing is a
      // TLS record this device cannot buffer plus a server that will not resume, and only
      // the second half is fixable (by the server sending Content-Length and honouring
      // Range). Same wording as the resume-budget path below, deliberately.
      if (lastHopTlsErr == WOLFSSL_MEMORY_E) {
        setDetail(sink.detail, "out of memory for TLS record at %zu bytes; server cannot resume", sink.downloaded);
      } else {
        setDetail(sink.detail, "incomplete: %zu/%zu bytes", sink.downloaded, sink.total);
      }
      return HttpDownloader::HTTP_ERROR;
    }
    // 416 answering a Range means our offset is at or past the end of the resource, i.e.
    // we already hold every byte. That only became reachable once unframed bodies could
    // resume: with no Content-Length there is nothing to compare against, so a body that
    // was truncated exactly at its final byte still arrives here with responseComplete()
    // false and resumes one hop too far. Treating it as an error would fail a download
    // that is, in fact, whole.
    if (resumeOffset > 0 && status == 416) {
      SdDebugLog::log("HTTP", "range not satisfiable at %zu -> body already complete (%zu bytes)", resumeOffset,
                      sink.downloaded);
      return HttpDownloader::OK;
    }
    if (status != 200 && status != 206) {
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
      // tlsErr names the cause instead of leaving it to inference: -125 (MEMORY_E) is
      // wolfSSL failing to allocate the receive buffer for an incoming record, which on
      // this device means the record was larger than the biggest free block; 0 means the
      // peer closed or the transport dropped and the heap is not implicated at all.
      // slabMiss > 0 alongside tlsErr=-125 means the reserved block was already taken when
      // wolfSSL asked, so the request went to malloc and lost; slabMiss=0 with tlsErr=-125
      // means the failing allocation was outside the record-size band this reserves.
      SdDebugLog::log("HTTP",
                      "incomplete: got %zu of %zu bytes after %lums (wait=%lums work=%lums) heap=%u largest8=%u "
                      "tlsErr=%d slabHit=%lu slabMiss=%lu",
                      sink.downloaded, sink.total, (unsigned long)(millis() - transferStartMs),
                      (unsigned long)waitTotalMs, (unsigned long)workTotalMs, s.heapFree, s.largest8Bit,
                      http.lastTlsError(), (unsigned long)freeink::TlsRecordSlab::hits(),
                      (unsigned long)freeink::TlsRecordSlab::misses());
      if (freeink::TlsRecordSlab::largestMiss() > 0) {
        SdDebugLog::log("HTTP", "tls slab largest miss=%lu (block=%u)",
                        (unsigned long)freeink::TlsRecordSlab::largestMiss(), (unsigned)freeink::TlsRecordSlab::size());
      }

      // Resume rather than throw the partial body away.
      //
      // On the X3 a 1.6MB book download dies mid-stream at ~215KB, every time, on a
      // healthy link: two attempts logged 215179/1676098 after 1742ms and
      // 216812/1676098 after 1783ms while running at ~121KB/s, and a 130KB feed to the
      // same host completed at 119KB/s in the same session. So it is neither slowness
      // nor a server response deadline (an earlier attempt died at 203242 bytes after
      // 26.7s — same bytes, wildly different time).
      //
      // The mechanism is the heap. With WiFi up the X3's largest free block is pinned
      // at 14324 bytes in every snapshot, before and after the handshake. wolfSSL sizes
      // its receive buffer to each incoming record (GrowInputBuffer), so the first
      // full-size 16KB TLS record the server emits needs a ~16.7KB contiguous
      // allocation that cannot exist, fails MEMORY_E, and kills the session mid-body.
      // Servers ramp record size as a connection warms up, which is why small feeds
      // finish and only a large download crosses the wall — and why a FRESH connection
      // gets another ~200KB before hitting it again. HAVE_MAX_FRAGMENT already asks for
      // 2KB records (SecureClient.cpp), but RFC 6066 max_fragment_length is advisory
      // and widely unimplemented (nginx has never supported it), so it cannot be relied
      // on. Nothing app-side can enlarge that 14KB block while the radio is up.
      //
      // Resuming from the byte offset works around it and is the right response to any
      // mid-stream drop whatever the cause. Guarded on forward progress, so a server
      // that fails at offset 0 ends the loop instead of spinning — and on CONVERGENCE
      // (see MIN_RESUME_HOP_BYTES), so a transfer that is advancing steadily is never
      // cut off just for being long.
      // A body with no Content-Length — chunked, or connection-delimited — leaves
      // sink.total at 0, and the guard here used to open with `sink.total > 0`. That
      // disabled resume ENTIRELY for those responses: the condition failed on its first
      // term, fell through to "resume budget spent", and reported resumes=0 having never
      // been allowed a single attempt. A device capture shows three consecutive downloads
      // of a dynamically generated article.epub dying at 214852, 217976 and 204755 bytes
      // "of 0" — the same mid-stream TLS drop described above, on the one response shape
      // that could not recover from it.
      //
      // Unknown length changes only what "not finished yet" means. It cannot be
      // `downloaded < total`, so completeness is decided where it already was: a hop that
      // ends with responseComplete() returns OK, and only a TRUNCATED hop reaches here.
      // Every other guard is unchanged and does the real work — strict forward progress,
      // stall convergence, the attempt ceiling, and the cancel flag.
      const bool lengthKnown = sink.total > 0;
      const size_t hopBytes = sink.downloaded - resumeOffset;
      if (sink.downloaded > resumeOffset && (!lengthKnown || sink.downloaded < sink.total) &&
          resumes < MAX_RESUME_ATTEMPTS && stalledResumes < MAX_RESUME_STALLS &&
          !(sink.cancelFlag && *sink.cancelFlag)) {
        stalledResumes = hopBytes < MIN_RESUME_HOP_BYTES ? stalledResumes + 1 : 0;
        ++resumes;
        resumeOffset = sink.downloaded;
        lastHopTlsErr = http.lastTlsError();  // the next hop's restart decision reads this
        retriedConnect = false;               // each resumed hop gets its own one-shot connect retry
        SdDebugLog::log("HTTP", "resuming at %zu/%zu (attempt %d/%d, hop=%zu stalls=%d)", resumeOffset, sink.total,
                        resumes, MAX_RESUME_ATTEMPTS, hopBytes, stalledResumes);
        delay(200);  // let the stack tear the dead socket down before reconnecting
        continue;
      }
      SdDebugLog::log("HTTP", "resume budget spent: got=%zu/%zu resumes=%d stalls=%d lastHop=%zu tlsErr=%d",
                      sink.downloaded, sink.total, resumes, stalledResumes, hopBytes, http.lastTlsError());
      // MEMORY_E means wolfSSL could not allocate the receive buffer for an incoming TLS
      // record — the server sends records larger than our biggest free block. Reporting
      // that as "incomplete" sends the user looking at their network, which is the one
      // thing that is fine: the same connection was moving at ~120KB/s when it died.
      if (http.lastTlsError() == WOLFSSL_MEMORY_E) {
        setDetail(sink.detail, "out of memory for TLS record at %zu bytes (file too large)", sink.downloaded);
      } else {
        setDetail(sink.detail, "incomplete: %zu/%zu bytes", sink.downloaded, sink.total);
      }
      return HttpDownloader::HTTP_ERROR;
    }
    // A complete hop that still leaves the resource short means the server answered a
    // bounded range (or closed exactly on a boundary); ask for the rest. Same
    // convergence guard as the truncation path — this branch has no inter-hop delay, so
    // a server answering tiny ranges would otherwise spin through the whole budget.
    const size_t shortHopBytes = sink.downloaded - resumeOffset;
    if (sink.total > 0 && sink.downloaded < sink.total && sink.downloaded > resumeOffset &&
        resumes < MAX_RESUME_ATTEMPTS && stalledResumes < MAX_RESUME_STALLS && !(sink.cancelFlag && *sink.cancelFlag)) {
      stalledResumes = shortHopBytes < MIN_RESUME_HOP_BYTES ? stalledResumes + 1 : 0;
      ++resumes;
      resumeOffset = sink.downloaded;
      retriedConnect = false;
      SdDebugLog::log("HTTP", "short range, resuming at %zu/%zu (attempt %d/%d, hop=%zu stalls=%d)", resumeOffset,
                      sink.total, resumes, MAX_RESUME_ATTEMPTS, shortHopBytes, stalledResumes);
      continue;
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
      // slabHit/slabMiss are the record-buffer allocations wolfSSL made: hits came from
      // the reserved block, misses fell through to malloc and are the ones that can still
      // fail with MEMORY_E. A healthy HTTPS transfer should show many hits and no misses;
      // hits=0 on an https:// URL means the allocator hook never saw the record buffer and
      // the diagnosis needs revisiting, not the sizing.
      SdDebugLog::log("DONE",
                      "bytes=%zu elapsed=%lums rate=%uB/s wait=%lums work=%lums resumes=%d slabHit=%lu slabMiss=%lu",
                      sink.downloaded, (unsigned long)totalElapsedMs, bytesPerSec, (unsigned long)waitTotalMs,
                      (unsigned long)workTotalMs, resumes, (unsigned long)freeink::TlsRecordSlab::hits(),
                      (unsigned long)freeink::TlsRecordSlab::misses());
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
  // A file destination can be emptied, so this transfer can start over at byte 0 when a
  // server turns out not to honour Range. HalFile has no truncate(); reopening for write
  // does truncate, and the close() first is required before any reopen of the same handle
  // (DESTRUCTOR_CLOSES_FILE only covers scope exit — see CLAUDE.md).
  struct RewindCtx {
    HalFile* file;
    const char* path;
  } rewindCtx{&file, destPath.c_str()};
  sink.rewindCtx = &rewindCtx;
  sink.rewind = [](void* ctx) {
    auto* rc = static_cast<RewindCtx*>(ctx);
    rc->file->close();
    if (Storage.openFileForWrite("HTTP", rc->path, *rc->file)) return true;
    LOG_ERR("HTTP", "rewind: cannot reopen %s", rc->path);
    return false;
  };

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
