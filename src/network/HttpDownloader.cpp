#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <SdDebugLog.h>
#include <base64.h>
#include <esp_wifi.h>

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
// Idle deadline for a RESUMED hop, which is a different bet from the first one.
//
// _timeoutMs in SecureHttpClient is an IDLE deadline, restamped on every successful read
// (readFixed/readUntilClose), not a total. The median productive hop in the 08-20 capture took
// 150ms, so 60 seconds of silence is not a slow server, it is a dead socket -- and six of them
// (five tlsErr=-397 SOCKET_PEER_CLOSED_E plus one silent hang) burned 182s, 26% of a 691s run.
//
// 15s rather than 5s because one hop in that same capture had ttfb=15833ms and DID deliver.
// Losing a hop costs one window and is re-requested; hop 1 keeps the full 60s because it has
// no offset to resume from and must learn Content-Length.
constexpr int RESUME_TIMEOUT_MS = 15000;
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
// stall threshold forever. Every hop must make strict forward progress, and the
// caller's cancel flag is polled per chunk, so a user can always abort.
//
// 512, arrived at in two measured steps on the same 24,139,108-byte https book.
//
// 128 -> 256: an X4 capture ran 18 hops at 137,937 B/hop, so 128 worked out to 17.7MB
// and would have failed the file at 73%.
//
// 256 -> 512: a full X3 capture then showed the per-hop figure is not a constant but a
// function of heap contiguity, and X3 sits far worse than X4. 139 hops delivered
// 13,880,662 bytes -- 99,861 B/hop, needing 242 hops for the file against a cap of 256.
// Six percent of margin, on a distribution that is BIMODAL rather than clustered: 62 of
// those hops fell in 50-75KB and only 22 in 200-225KB, tracking `largest8` at connect
// time. A run that lands in the low mode more often blows straight through 256 -- at the
// smallest hop actually observed (51,731 bytes) the file needs 467. 512 covers that.
//
// Contiguity also erodes as a download proceeds, which is why the low mode is not rare:
// across that capture free heap held flat (12,339 -> 11,822 comparing the first 40 hops
// to the last 40) while largest8 fell 3,572 -> 2,632. Fragmentation, not a leak, so the
// budget has to survive hops getting shorter rather than assume an average.
//
// Raising it stays cheap because this ceiling is not what bounds a bad server:
// MAX_RESUME_STALLS ends a non-converging transfer after 4 consecutive short hops no
// matter how high this sits, and every hop must make strict forward progress. What the
// ceiling bounds is wall time against a server dribbling just ABOVE the stall threshold
// -- and the measured per-hop cost is ~1.36s of connect, of which only ~57ms is the TLS
// handshake (session tickets) and ~15ms is TCP. The other ~1,285ms is the server's
// time-to-first-byte on a Range request, i.e. 95% of a hop is the peer thinking. Nothing
// here can shorten that; the ceiling only decides how long we are willing to keep trying,
// and the caller's cancel flag is polled per chunk so a user can always stop it.
//
// 512 -> 1024 (08-20), off the first capture in which large https downloads actually
// finished on X3. Eight font files of 739694..1306713 bytes needed 165..302 attempts, i.e.
// ~242 attempts per MB once empty hops are counted -- so 512 covers about 2.2MB and the
// largest file measured already used 59% of it. 1024 covers ~4.4MB, which clears every font
// in the manifest with margin. The per-attempt cost is unchanged and so is the bad-server
// bound: MAX_EMPTY_HOPS_RECORD_WALL and MAX_RESUME_STALLS end a non-converging transfer long
// before this ceiling is reached.
constexpr int MAX_RESUME_ATTEMPTS = 1024;
// ...but a flat ceiling IS a cap on file size, and a bounded Range window shrinks the bytes
// per hop on purpose, so the ceiling has to follow the file. At a 12KB window a 24MB OPDS book
// needs ~2,000 hops and 1024 would fail it at 51%. One attempt per 4KB plus the flat 1024
// covers the floor case with margin (1.26MB -> 1,331; 24MB -> 7,168) and is still bounded.
// Unknown length keeps the flat value, since there is nothing to derive from.
constexpr int MAX_RESUME_ATTEMPTS_CAP = 16384;
// 8KB, and it must stay BELOW what one TLS record's worth of body can deliver.
//
// This was 16 * 1024, which is above it, and that one kilobyte capped every https transfer
// on a tight heap at four resumed hops no matter how much attempt budget was left. When
// largest8 after the handshake sits under the ~16.4KB wolfSSL needs for a full-size record
// (see the truncation handler), a hop delivers whatever arrives before the first
// full-size record and then dies MEMORY_E. Measured on X3 against GitHub's release CDN
// (opds_debug.txt): hops of 15420, 15420 and 15415 bytes -- one 16KB record minus framing,
// every time, and every one of them under 16384. So each was scored a stall, stalledResumes
// never reset, and MAX_RESUME_STALLS ended a 739694-byte font at 63608 bytes with 509
// attempts unspent.
//
// The threshold is meant to separate "converging" from "dribbling". A hop yielding one
// whole TLS record is converging -- it is the most this heap can carry -- so the line has
// to sit below it. 8KB leaves room for a smaller record wall on a worse heap while staying
// far above the few-hundred-byte hops that mean a server really is stuck.
constexpr size_t MIN_RESUME_HOP_BYTES = 8 * 1024;
constexpr int MAX_RESUME_STALLS = 4;
// Bounded-Range window: how many bytes a resumed hop asks for, instead of "everything from
// here". This is the lever that actually removes the TLS record wall, and it exists because
// wolfSSL FREES its input buffer after every record it processes
// (ShrinkInputBuffer(ssl, NO_FORCED_FREE), Arduino-wolfSSL internal.c:22107 and :22166,
// unconditional when dynamicFlag is set). So every incoming record needs its own fresh
// CONTIGUOUS allocation of the record's own size, and on this heap a 16,401-byte one almost
// always fails.
//
// The peer picks the record size, but the record can never be bigger than what the peer has
// to write. An open-ended `Range: bytes=N-` invites a 16KB stream; a bounded
// `Range: bytes=N-M` makes the whole response headers+body, which for a small window is one
// small record and one allocation this heap can serve.
//
// Measured on X3 against GitHub's release CDN (opds_debug.txt, 08-20): 1,478 resumed hops for
// four fonts, of which 1,463 died with tlsErr=-125 having delivered NOTHING -- 87%. Empty hops
// log no CONNECT line (CONNECT is emitted on the first body byte), so the 206 headers arrived
// in their own small record and the FIRST BODY record is what failed. The productive hops are
// the tell: 15,414-15,444 bytes, every single one, which is 16,384 minus ~944 of 206 headers.
// Exactly one record per connection is all this heap sustains.
//
// 12KB to start: 12,288 + ~900 of headers = ~13,188 in one record, ~20% under the 16,401 that
// is failing, without pretending to know the heap's true ceiling -- the adaptation below finds
// it. 4KB floor still moves 29KB/s at the measured ~140ms per hop. 64KB ceiling because hop 1,
// which never has a window, has been measured delivering 49,152-64,509 bytes on a fresh heap.
constexpr size_t RANGE_WINDOW_START = 12 * 1024;
constexpr size_t RANGE_WINDOW_MIN = 4 * 1024;
constexpr size_t RANGE_WINDOW_MAX = 64 * 1024;
// Consecutive full-window hops before the window grows. Growth is damped and shrink is not,
// ON PURPOSE: success here is probabilistic, so a symmetric rule hunts across the boundary
// forever instead of settling just under it. Halve on one failure, grow after four wins.
constexpr int RANGE_WINDOW_GROW_RUN = 4;
// Consecutive hops that delivered ZERO bytes, counted separately from the short-hop stalls
// above and reset by any hop that carries even one byte.
//
// A short hop and an empty hop are different signals and sharing one counter conflated
// them. On the capture above a resumed hop either cleared one TLS record or delivered
// nothing at all, roughly half and half, depending on how the server packed headers and
// body into records -- the empty ones are not a failing transfer, they are the same wall
// landing one record earlier. Charging them to the same budget as a dribbling server meant
// a long download could not survive its own hop distribution: at that rate the ~48 hops a
// 739694-byte file needs will contain a run of four with near-certainty.
//
// 8 is sized off that: an empty-hop rate of one in two needs a run of eight before it gives
// up, which over 48 hops is unlikely rather than near-certain. The cost is bounded and
// small -- a server that has genuinely stopped answering spends 8 backoffs (~18s, sliced
// and cancel-polled) instead of 4 before the error appears.
constexpr int MAX_EMPTY_HOPS = 8;
// Ceiling on the linear backoff between retried hops. Without it the empty-hop ladder above
// would reach 4s on its last step, which is a long time to hold a progress bar still for a
// hop that usually succeeds on the next try.
constexpr uint32_t MAX_RESUME_BACKOFF_MS = 2000;
// An empty hop that died of MEMORY_E is the record wall, and the wall does not heal with
// time — so it gets its own, much larger ceiling and a flat delay instead of the ladder.
//
// Measured on X3 against GitHub's release CDN (opds_debug.txt, 51 empty hops): every single
// one reported largest8=17396, identical to four significant figures, whatever the preceding
// backoff was. Nothing about the heap changes while we wait. The recovery rate confirms it
// from the other side -- P(the next attempt carries bytes) by ladder position ran 29%, 25%,
// 11%, 12%, 29%, 40%, 0%, 33%, i.e. flat noise around 27% with no trend against a backoff
// that grew 4x across those positions. The wait was pure latency.
//
// What actually decides an empty hop is server-side framing: the same Range re-requested
// gets a different record layout, and roughly one attempt in four opens with a record small
// enough to buffer. So the right response is to ask again promptly, not to wait.
//
// 40, and the step from 24 is measured, not padding. The 24-run capture completed all eight
// font files (739694..1306713 bytes, 165..302 resumes each) but one position in Vollkorn_16
// consumed exactly 24 empties and recovered on the last attempt it was allowed -- zero
// margin. Across ~180 hop positions in a four-file family that ceiling is P(family
// completes) = 87.7%, i.e. roughly one install in eight fails at the very end. 40 takes the
// per-position failure from 0.073% to 0.0006% and the family from 87.7% to 99.9%.
//
// At the original ceiling of 8 the same arithmetic gives 8.1% per position and a 2.3% chance
// of finishing one 698158-byte file -- and indeed two positions in the pre-fix capture
// exhausted 8 and killed files that were 43% and 6% delivered.
//
// It is affordable only because the flat delay replaces the ladder: 40 attempts at
// EMPTY_HOP_WALL_DELAY_MS plus a ~330ms hop is ~14s of worst case, which is exactly what 8
// rungs of the old ladder already cost for a fifth of the attempts (measured twice, 14.288s
// and 14.281s). Typical cost is far lower -- the median run recovers in 2 attempts.
constexpr int MAX_EMPTY_HOPS_RECORD_WALL = 40;
// Flat delay before re-asking for a range that came back empty on the record wall.
//
// Small on purpose. Waiting does not improve the odds: across 1,165 hops of one four-font
// family, the chance the next hop delivers bytes is flat at 0.16-0.18 whether the gap before
// it was under 250ms or over 1.5s. At 200ms the 968 empty hops in that capture spent 194 of
// its 958 seconds asleep for nothing.
//
// Not zero either: the socket from the dead hop still needs tearing down (the same reason the
// truncation path delays), and the loop must yield to the WiFi task. A hop costs ~330ms of
// real work on its own, so this does not change the request rate the CDN sees in any way that
// matters -- ~2.8/s against ~1.9/s.
constexpr uint32_t EMPTY_HOP_WALL_DELAY_MS = 25;
// Re-issues allowed for a FRESH hop that answered 200/206 and then delivered nothing. Bounded
// hard because nothing advances between attempts -- there is no offset to move and no counter
// to reset, so this is the only thing standing between an unlucky first hop and a spin.
//
// Three is right for a hop that failed for any reason OTHER than the record wall -- same
// reasoning as MAX_RANGE_RESTARTS, where a couple of retries convert the user's manual "try
// again" into one operation and more would just burn radio time.
//
// MEMORY_E gets ten, for the same reason the resumed ladder gets forty: it is not a broken
// server, it is the record wall, and the budget has to be sized off the measured empty rate
// rather than intuition. Fresh hops came up empty on 3 of 7 attempts across one four-file
// family (p = 0.43), and at a ceiling of 3 that is a 7.9% chance of losing a file on its very
// first hop -- 28% across a family. The capture shows exactly that near-miss:
// SourceCodePro_12.cpfont spent all three retries and succeeded on the last one it was
// allowed. Ten takes per-file failure to 0.02% and costs 3.5s of worst case.
constexpr int MAX_FRESH_RETRIES = 3;
// Free heap below which a wolfSSL handshake cannot be expected to complete.
//
// Measured, not guessed. In the 08-20 NotoSerifExtended capture every one of 54 failed
// handshakes reported 29,640-35,504 bytes free, while the files that succeeded had started
// from 41,376-41,456. SecureClient puts a TLS 1.3 handshake at ~35-43KB of small allocations,
// and its MEMFIX-PORT note records the same failure from the other side: a keygen allocation
// fails and the ClientHello is never sent.
//
// The signature is exact and worth learning: tcp>0 (the socket opened), tls=0 (the handshake
// never completed), tlsErr=0 (no read error, because nothing was ever read). That is
// starvation, not a peer or a link problem, and retrying does not fix it.
constexpr uint32_t TLS_HANDSHAKE_MIN_FREE = 36 * 1024;
// Connect attempts allowed once starvation is identified. Two, not MAX_EMPTY_HOPS: by then the
// heap is all we are going to get, and each further attempt is measurably destructive — every
// failed handshake costs ~190 bytes that do not come back, so an exhausted 8-rung ladder (18
// attempts) burned ~3.4KB and dragged the NEXT file's GET start down with it (41,456 -> 38,100
// -> 37,888). That ratchet is why the first starved file used to poison a whole session.
constexpr int MAX_STARVED_CONNECTS = 2;
constexpr int MAX_FRESH_RETRIES_RECORD_WALL = 10;
// Full restarts allowed when a server answers 200 to a Range request (i.e. it does
// not support resuming at all). Two, because a restart is only worth attempting while
// a fresh connection still has a real chance: the same 130676-byte feed completed
// outright on 2 of 5 attempts in one session, so a couple of retries convert the
// user's manual "try again, try again" into one operation, while more than that would
// just burn radio time on a link that clearly cannot hold the transfer.
constexpr int MAX_RANGE_RESTARTS = 2;
// How long to wait for the station to re-associate before treating a dead connect as a
// transfer failure, and how many times per transfer. 15s covers a normal reassociation;
// 3 recoveries keeps a genuinely lost AP from holding the screen indefinitely, while still
// protecting a multi-megabyte partial across the kind of dropout that discarded 2,611,177
// bytes in the X3 capture. See the link-down branch in runGet.
constexpr uint32_t LINK_WAIT_MS = 15000;
constexpr int LINK_WAIT_LIMIT = 3;

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
  // Link-down recoveries used by this transfer (see LINK_WAIT_LIMIT).
  int linkWaits = 0;
  int redirects = 0;
  // Byte offset a resumed hop asks the server to continue from (0 = fresh request).
  // See the truncation handler below for why a mid-stream drop is resumed rather
  // than failed.
  size_t resumeOffset = 0;
  int resumes = 0;
  // Hops that advanced less than MIN_RESUME_HOP_BYTES, in a row. Reset by any hop that
  // makes real progress, so a converging transfer is never cut off by its own length.
  int stalledResumes = 0;
  // Bytes a resumed hop asks for, or 0 for an open-ended `Range: bytes=N-`.
  //
  // Starts at 0 -- today's behaviour -- and is armed ONLY by a hop that came back empty with
  // MEMORY_E. That single condition is what makes this incapable of regressing anything else:
  // plain HTTP never enters wolfSSL (SecureHttpClient runs it over the WiFiClient transport),
  // so it can never raise MEMORY_E, so it can never acquire a window and keeps streaming at the
  // 233KB/s it already does. An OPDS feed finishes in hop 1 and never resumes at all.
  size_t rangeWindow = 0;
  // Full-window hops in a row. See RANGE_WINDOW_GROW_RUN for why growth is damped.
  int windowWins = 0;
  // Set by the first hop that actually delivers a whole window, i.e. proof the peer honours a
  // range END and frames it in a record this heap can hold.
  bool windowProved = false;
  // Set when the window reached its floor without ever being proved, which means bounding the
  // range did not change the peer's framing at all. The window is then abandoned for the rest
  // of the request and never re-armed, so the worst case of this whole mechanism is a handful
  // of wasted hops followed by EXACTLY today's open-ended behaviour -- never a download that
  // used to finish slowly and now fails.
  bool windowFutile = false;
  // Attempt ceiling, re-derived whenever the resource size is learned. See
  // MAX_RESUME_ATTEMPTS_CAP for why a flat number cannot serve both a 1MB font and a 24MB book
  // once the window shrinks the bytes each hop carries.
  int resumeBudget = MAX_RESUME_ATTEMPTS;

  // A hop that carried the whole window is one win; RANGE_WINDOW_GROW_RUN of them in a row
  // widen it. Anything short resets the run without shrinking -- only the record wall shrinks.
  auto noteWindowWin = [&](size_t hopBytes) {
    if (rangeWindow == 0) return;
    if (hopBytes < rangeWindow) {
      windowWins = 0;
      return;
    }
    windowProved = true;
    if (++windowWins < RANGE_WINDOW_GROW_RUN) return;
    windowWins = 0;
    const size_t before = rangeWindow;
    rangeWindow = rangeWindow * 3 / 2 > RANGE_WINDOW_MAX ? RANGE_WINDOW_MAX : rangeWindow * 3 / 2;
    if (rangeWindow != before)
      SdDebugLog::log("HTTP", "range window %zu -> %zu (%d clean hops)", before, rangeWindow, RANGE_WINDOW_GROW_RUN);
  };
  // Three ways a hop can carry nothing, counted SEPARATELY and each reset by any hop that
  // carries a byte. One shared counter looks tidier and is wrong: the ceilings differ (40 for
  // the record wall, 8 for the rest), so a shared count can reach a run of, say, 12
  // legitimate wall retries and then fail the very next `< MAX_EMPTY_HOPS` test the moment
  // the cause changes. A device capture (08-20, OpenDyslexic) has runs of 15 wall empties AND
  // a WiFi dropout inside the same transfer; they merely did not overlap. Had they, a
  // converging download with 25 wall retries still owed would have died telling the user the
  // server had stopped responding.
  int emptyWallHops = 0;    // opened, answered, delivered nothing, tlsErr == MEMORY_E
  int emptyOtherHops = 0;   // opened, answered, delivered nothing, any other cause
  int connectFailures = 0;  // never opened at all
  // Every empty hop this request has seen, whatever the cause, never reset. Purely for the
  // logs: the three counters above are CONSECUTIVE counts, so by the time a transfer succeeds it has been zeroed by the
  // winning hop and reporting it reads as "the record wall was never hit". A capture of four font files that took 258
  // empty hops between them printed empty=0 on all four DONE lines.
  int emptyHopsTotal = 0;
  // Re-issues of a fresh (unresumed) hop that opened and then delivered nothing. Never
  // reset: nothing advances between those attempts, so this counter is the only bound.
  int freshRetries = 0;
  // Whole-request clock and socket/work totals, summed across every redirect and resume hop.
  // The per-hop copies inside the loop are reset on each hop's first body byte, which is what
  // the "incomplete:" line wants and what "DONE:" must NOT use — see the DONE log site.
  const uint32_t requestStartMs = millis();
  uint32_t waitAllMs = 0;
  uint32_t workAllMs = 0;
  // Full restarts spent because the server answered 200 to a Range request.
  int rangeRestarts = 0;
  // wolfSSL error that ended the PREVIOUS hop, 0 for a hop that ended cleanly. Read by the
  // range-restart decision below, which must know whether the last failure was one a fresh
  // connection could plausibly avoid.
  int lastHopTlsErr = 0;

  // wolfSSL's per-record receive buffer is NOT backed by a reserved block here, and must not
  // be. TlsRecordSlab was sized (5120) for a record ramp measured against a different origin,
  // where a hop died on the step to 4,246-byte records. release-assets.githubusercontent.com
  // does not ramp: it goes straight to 16,401, so every capture logs slabHit=0 with
  // maxMissEver=16401 — the block is bought, never handed out once, and the 5,120 bytes it
  // takes come out of the largest free block, which is the one resource the 16,401-byte
  // allocation actually needs (largest8 22,516 -> 17,396).
  //
  // Measured over 1,148 hops of one four-font family, comparing the segments where the block
  // happened to be held against those where it had been handed back:
  //   held: largest8 17,396, 84% of hops empty, mean hop 17,428 B, 7,214 B/s
  //   off : largest8 22,516, 76% of hops empty, mean hop 23,652 B, 12,325 B/s
  // Leasing it costs 1.7x throughput. Re-enable only against an origin whose records are
  // proven to sit in the 4-5KB band, and only on that origin's evidence.

  // Connect attempts made after a handshake was identified as starved.
  int starvedConnects = 0;

  for (;;) {
    freeink::SecureHttpClient http;
    http.setTimeout(resumeOffset > 0 ? RESUME_TIMEOUT_MS : HTTP_TIMEOUT_MS);
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      setDetail(sink.detail, "bad URL");
      return HttpDownloader::HTTP_ERROR;
    }
    if (resumeOffset > 0) {
      if (rangeWindow > 0) {
        // Clamp to the last byte of the resource when the length is known. A range END past
        // the end is legal and servers clamp it themselves (RFC 9110 14.1.2), but asking for
        // exactly what is left keeps the final hop's Content-Length honest and avoids relying
        // on that behaviour.
        size_t last = resumeOffset + rangeWindow - 1;
        if (sink.total > 0 && last >= sink.total) last = sink.total - 1;
        http.addHeader("Range", "bytes=" + std::to_string(resumeOffset) + "-" + std::to_string(last));
      } else {
        http.addHeader("Range", "bytes=" + std::to_string(resumeOffset) + "-");
      }
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
            emptyWallHops = 0;
            emptyOtherHops = 0;
            connectFailures = 0;
          }
          if (!loggedConnect) {
            loggedConnect = true;
            transferStartMs = millis();
            lastChunkMs = transferStartMs;
            // A 206's Content-Length is the length of the RANGE, not of the resource,
            // so never let it overwrite the full size learned on the first hop. The
            // sink.total == 0 guard already covers this (a resume only happens once
            // total is known); resumeOffset makes the intent explicit.
            if (sink.total == 0 && resumeOffset == 0 && http.hasContentLength()) {
              sink.total = http.getContentLength();
              const size_t derived = MAX_RESUME_ATTEMPTS + sink.total / 4096;
              resumeBudget = static_cast<int>(derived > MAX_RESUME_ATTEMPTS_CAP ? MAX_RESUME_ATTEMPTS_CAP : derived);
            }
            const SdDebugLog::NetSnapshot snap = SdDebugLog::captureNetSnapshot();
            // handshake= is the whole open, first byte to first byte. Split it: with
            // session tickets the TLS leg is ~90ms but the whole open measured ~1350ms on
            // X4, so ~1250ms per hop belongs to something else, and over the ~177 hops a
            // 24MB book needs that is minutes. tcp covers DNS + SYN (both inside
            // WiFiClient::connect); ttfb is the remainder -- request write plus the
            // server's think time seeking to the Range offset -- and is the only leg here
            // that is not ours to fix. tcp/tls read 0 on a plain-HTTP hop, where the
            // whole open is ttfb by definition.
            const uint32_t openMs = transferStartMs - openStartMs;
            const uint32_t tcpMs = http.tcpConnectMs();
            const uint32_t tlsMs = http.tlsHandshakeMs();
            const uint32_t ttfbMs = openMs > tcpMs + tlsMs ? openMs - tcpMs - tlsMs : 0;
            SdDebugLog::log("CONNECT",
                            "handshake=%lums tcp=%lums tls=%lums ttfb=%lums resumed=%d heap=%u largest8=%u intFree=%u "
                            "intLargest=%u rssi=%d total=%zu url=%s",
                            (unsigned long)openMs, (unsigned long)tcpMs, (unsigned long)tlsMs, (unsigned long)ttfbMs,
                            http.tlsSessionResumed() ? 1 : 0, snap.heapFree, snap.largest8Bit, snap.internalFree,
                            snap.internalLargest, (int)snap.rssi, sink.total, url.c_str());
          }

          // Time waiting on the socket since the PREVIOUS callback returned. lastChunkMs
          // is stamped at the END of this callback, not here: stamping it on entry folded
          // our own SD write and the caller's progress repaint into the next "stall", so
          // the two were indistinguishable — a 130KB OPDS feed logged 34.8s of gaps that
          // may have been largely e-ink refreshes, not the link.
          const uint32_t now = millis();
          const uint32_t gapMs = now - lastChunkMs;
          waitTotalMs += gapMs;
          waitAllMs += gapMs;
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
          workAllMs += workMs;
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
      // rssi is on this line because its ABSENCE is the diagnosis. captureNetSnapshot
      // reports 0 when esp_wifi_sta_get_ap_info() fails, i.e. when the station is not
      // associated — and without it a capture full of "failed after 4ms" is unreadable.
      // 4ms is far too fast for a TCP connect to have been attempted, so the radio, not
      // the peer, is the suspect; this is what turns that suspicion into a fact.
      // tcp/tls/tlsErr split the three ways GET() returns -1, which this line could not
      // previously tell apart — and that ambiguity cost a whole debugging round. SecureClient
      // sets _tcpMs only once the TCP connect (DNS included) has succeeded and _tlsMs only
      // once the handshake has COMPLETED, so:
      //   tcp=0 tls=0  -> DNS or the TCP SYN failed; the radio or the route is the suspect
      //   tcp>0 tls=0  -> TCP is fine, the TLS handshake failed; read tlsErr
      //   tcp>0 tls>0  -> both fine, so the peer accepted TLS and then refused or dropped
      //                   the HTTP exchange — a server-side rejection, not a device problem
      // The third case is invisible without this and looks identical to the first in a
      // capture, which is exactly the confusion to avoid: on X3 a link-down failure returns
      // in ~4ms while a post-handshake rejection takes ~180ms, and only the split says which.
      SdDebugLog::log("HTTP",
                      "wolfSSL request failed after %lums (tcp=%lums tls=%lums tlsErr=%d) heap=%u largest8=%u rssi=%d "
                      "url=%s",
                      (unsigned long)(millis() - openStartMs), (unsigned long)http.tcpConnectMs(),
                      (unsigned long)http.tlsHandshakeMs(), http.lastTlsError(), s.heapFree, s.largest8Bit, (int)s.rssi,
                      url.c_str());
      // A handshake that never completed, at a heap too small to run one, is starvation —
      // and it is the one connect failure that retrying makes actively WORSE.
      //
      // Measured on X3 (08-20, NotoSerifExtended): three files of 703KB, 880KB and 1.08MB
      // completed, then every connect failed with tcp>0 tls=0 tlsErr=0 at 29,640-35,504 bytes
      // free. Each failed attempt costs ~190 bytes that do not come back, so an exhausted
      // 8-rung ladder (18 attempts) burned ~3.4KB — and the loss carried into the NEXT file,
      // whose GET start fell 41,456 -> 38,100 -> 37,888. That is a one-way ratchet: the first
      // starved file drags the whole session below the threshold and every file after it
      // fails too, which is exactly the "it keeps failing" the retries were meant to prevent.
      //
      // So stop early rather than ratchet the heap down for the files that follow. Dropping
      // the record slab (see runGet's opening note) already returned 5,120 of those bytes to
      // every hop, which is what makes two attempts enough. This deliberately overrides the
      // connect ladder below — that ladder is right for a transport failure (a dropped association
      // returns in ~4ms with tcp=0 and recovers), and wrong for this one.
      if (http.tcpConnectMs() > 0 && http.tlsHandshakeMs() == 0 && s.heapFree < TLS_HANDSHAKE_MIN_FREE) {
        if (++starvedConnects >= MAX_STARVED_CONNECTS) {
          SdDebugLog::log("HTTP", "handshake starved at %u free (< %u), giving up after %d attempts", s.heapFree,
                          (unsigned)TLS_HANDSHAKE_MIN_FREE, starvedConnects);
          setDetail(sink.detail, "not enough memory for a secure connection (%u bytes free)", s.heapFree);
          return HttpDownloader::HTTP_ERROR;
        }
      }

      // "This hop delivered nothing" — the condition both recovery paths below need, and
      // NOT the same as "the transfer holds no bytes". resumeOffset and sink.downloaded
      // are set equal at the top of every hop (by both resume branches below and by the
      // range-restart rewind) and only the body callback moves them apart. Re-issuing
      // such a hop is idempotent: it carries `Range: bytes=<resumeOffset>-` and appends
      // exactly what the dead hop would have, so there is no duplicate to fear. On hop 1
      // resumeOffset is 0 and this is the original `sink.downloaded == 0` unchanged.
      const bool hopDeliveredNothing = sink.downloaded == resumeOffset;

      // A dropped WiFi association is not a server failure, and must not be charged to the
      // resume budget.
      //
      // Measured on X3: a download holding 2,611,177 bytes with 487 attempts unspent was
      // discarded after eight consecutive "wolfSSL request failed after 4ms". Four
      // milliseconds cannot contain a TCP connect, so nothing was ever attempted — and the
      // capture's surrounding lines show the link collapsing (rssi -75 to -91, and a feed
      // fetch minutes later needing tcp=4020ms to connect). The retry ladder above spent
      // the whole stall budget in seven seconds against a radio that was not associated,
      // then told the user the server had stopped responding.
      //
      // So wait for the link instead of consuming the budget. Bounded by LINK_WAIT_LIMIT
      // recoveries per transfer, cancel-polled throughout, and charged nothing when it
      // succeeds: a hop that never opened carried no bytes, so re-issuing it is idempotent
      // exactly as the retry below is.
      if (s.rssi == 0 && hopDeliveredNothing && linkWaits < LINK_WAIT_LIMIT) {
        ++linkWaits;
        SdDebugLog::log("HTTP", "link down at %zu bytes, waiting up to %lums (%d/%d)", sink.downloaded,
                        (unsigned long)LINK_WAIT_MS, linkWaits, LINK_WAIT_LIMIT);
        bool linkBack = false;
        for (uint32_t waited = 0; waited < LINK_WAIT_MS; waited += 250) {
          if (sink.cancelFlag && *sink.cancelFlag) return HttpDownloader::ABORTED;
          delay(250);
          if (SdDebugLog::captureNetSnapshot().rssi != 0) {
            linkBack = true;
            break;
          }
        }
        SdDebugLog::log("HTTP", "link %s", linkBack ? "back" : "still down");
        if (linkBack) {
          retriedConnect = false;  // the recovered hop gets its own one-shot immediate retry
          continue;
        }
      }

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
      // Charged to connectFailures, its own zero-byte budget, and not to the short-hop stalls
      // or to either empty-hop counter: this hop carried nothing, which is the same event the
      // truncation path handles when a connection opens and then dies before its first body
      // byte. Any hop that carries a byte resets it, so a long download that hiccups once
      // every twenty hops never accumulates toward the ceiling.
      //
      // This deliberately does NOT require resumeOffset > 0. It used to, on the reasoning
      // that "a server refusing the very first connect should fail fast rather than after a
      // ladder of backoffs" — and a capture on a weak link (rssi -70..-79) showed what that
      // costs. Four font downloads across three families each got exactly two attempts, the
      // initial connect and the one-shot retry above, both failing the TLS handshake after
      // ~200ms, all four dead inside 400ms with the whole resume machinery untouched. The
      // same session had just pulled the manifest from the SAME CDN host with
      // handshake=931ms, i.e. more than four times the window those connects were given
      // before being written off. A first hop is not special: it is a hop that carried
      // nothing, and it should get the same bounded ladder every other such hop gets.
      //
      // Bounded without the offset guard because connectFailures is reset ONLY by a hop that
      // carries a byte. On a first hop nothing carries a byte, so the limit alone bounds it —
      // the same argument that makes freshRetries safe on the truncation path below.
      if (hopDeliveredNothing && connectFailures < MAX_EMPTY_HOPS && resumes < resumeBudget &&
          !(sink.cancelFlag && *sink.cancelFlag)) {
        ++connectFailures;
        ++emptyHopsTotal;
        if (resumeOffset > 0) ++resumes;  // a first-hop retry is not a resume; keep DONE's stat honest
        retriedConnect = false;           // the next hop gets its own one-shot immediate retry
        // lastHopTlsErr is deliberately NOT touched: it records the error of the last hop
        // that actually moved bytes, which is what the range-restart decision reads. A
        // connect that never opened says nothing about record sizes.
        const uint32_t rawBackoffMs = 500u * static_cast<uint32_t>(connectFailures);
        const uint32_t backoffMs = rawBackoffMs > MAX_RESUME_BACKOFF_MS ? MAX_RESUME_BACKOFF_MS : rawBackoffMs;
        SdDebugLog::log("HTTP", "connect failed at %zu bytes, backing off %lums (connectFail %d/%d, attempt %d/%d)",
                        sink.downloaded, (unsigned long)backoffMs, connectFailures, MAX_EMPTY_HOPS, resumes,
                        resumeBudget);
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
      if (s.rssi == 0) {
        // Never blame the server for a radio that is not associated — the previous wording
        // sent the user to check a server that had done nothing wrong.
        setDetail(sink.detail, "WiFi connection lost at %zu bytes", sink.downloaded);
      } else if (resumeOffset > 0) {
        setDetail(sink.detail, "connection lost at %zu bytes; server stopped responding", sink.downloaded);
      } else if (connectFailures > 0) {
        // Reached only after the whole connect ladder was spent, so say so: "connect/read
        // failed" reads as a one-off blip and sends the user to check a link that the
        // rssi on the log line above shows was fine. Naming the attempt count also
        // distinguishes this from a first-try failure at a glance.
        setDetail(sink.detail, "no response after %d connection attempts", connectFailures + 1);
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
      // largest8 is the number that matters next to a -125: it is what the record buffer has
      // to fit in, and 16,401 is what this CDN asks for.
      SdDebugLog::log("HTTP",
                      "incomplete: got %zu of %zu bytes after %lums (wait=%lums work=%lums) heap=%u largest8=%u "
                      "tlsErr=%d",
                      sink.downloaded, sink.total, (unsigned long)(millis() - transferStartMs),
                      (unsigned long)waitTotalMs, (unsigned long)workTotalMs, s.heapFree, s.largest8Bit,
                      http.lastTlsError());

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
      // Every other guard is unchanged and does the real work — forward progress, stall
      // convergence, the attempt ceiling, and the cancel flag.
      const bool lengthKnown = sink.total > 0;
      const size_t hopBytes = sink.downloaded - resumeOffset;
      const bool moreToFetch = !lengthKnown || sink.downloaded < sink.total;
      if (sink.downloaded > resumeOffset && moreToFetch && resumes < resumeBudget &&
          stalledResumes < MAX_RESUME_STALLS && !(sink.cancelFlag && *sink.cancelFlag)) {
        // A window we chose is not a server dribbling. Score the stall against a quarter of
        // the window instead: still catches a peer that answers a 12KB range with 200 bytes,
        // but cannot kill a download for the crime of asking for less on purpose.
        const size_t progressFloor = rangeWindow > 0 ? rangeWindow / 4 : MIN_RESUME_HOP_BYTES;
        stalledResumes = hopBytes < progressFloor ? stalledResumes + 1 : 0;
        emptyWallHops = 0;  // this hop carried bytes
        emptyOtherHops = 0;
        connectFailures = 0;
        ++resumes;
        resumeOffset = sink.downloaded;
        lastHopTlsErr = http.lastTlsError();  // the next hop's restart decision reads this
        retriedConnect = false;               // each resumed hop gets its own one-shot connect retry
        SdDebugLog::log("HTTP", "resuming at %zu/%zu (attempt %d/%d, hop=%zu stalls=%d)", resumeOffset, sink.total,
                        resumes, resumeBudget, hopBytes, stalledResumes);
        noteWindowWin(hopBytes);
        // Same teardown as the empty-hop path and worth the same: the server closed, so
        // there is no TIME_WAIT on our side to wait out. 200ms here was 40s of the 958s
        // that capture spent on four fonts, all of it blocking the UI.
        delay(EMPTY_HOP_WALL_DELAY_MS);
        continue;
      }

      // A resumed hop that opened, answered 206 and then delivered NOTHING is not the end
      // of the download either — it is the record wall landing one record earlier than
      // usual, and the guard above rejected it only for failing strict forward progress.
      //
      // Measured on X3 against GitHub's release CDN (opds_debug.txt): four font files, and
      // every one of them ended exactly here. Vollkorn_12.cpfont carried 32768, 15420 and
      // 15420 bytes over three clean hops, then a fourth opened (no CONNECT line, so the
      // body callback never fired), answered 206, and died with tlsErr=-125 having moved
      // nothing. That hop failed `sink.downloaded > resumeOffset`, fell straight through to
      // "resume budget spent", and threw away 63608 good bytes with 509 attempts and 2
      // stall slots unspent. Roughly half the resumed hops in that capture were empty, so
      // treating the first one as fatal capped every large https download on this heap.
      //
      // Retrying is idempotent for exactly the reason the connect-failure path above is:
      // resumeOffset and sink.downloaded are equal, so the re-issued hop carries the same
      // `Range: bytes=<resumeOffset>-` and appends exactly what the dead hop would have.
      //
      // resumeOffset > 0 is required, and is what keeps this bounded: without it a fresh
      // hop 1 that delivers nothing would re-issue itself forever, since there is no offset
      // to advance and nothing to reset the counter.
      // The record wall gets a bigger budget and a flat delay; anything else keeps the
      // ladder. See MAX_EMPTY_HOPS_RECORD_WALL for the measurement behind the split — in
      // short, a MEMORY_E hop is a local allocation failure that a wait cannot heal, while
      // an empty hop from any other cause may well be the far end needing a moment.
      const bool hitRecordWall = http.lastTlsError() == WOLFSSL_MEMORY_E;
      const int emptyLimit = hitRecordWall ? MAX_EMPTY_HOPS_RECORD_WALL : MAX_EMPTY_HOPS;
      const int emptyRun = hitRecordWall ? emptyWallHops : emptyOtherHops;
      if (resumeOffset > 0 && sink.downloaded == resumeOffset && moreToFetch && emptyRun < emptyLimit &&
          resumes < resumeBudget && !(sink.cancelFlag && *sink.cancelFlag)) {
        ++(hitRecordWall ? emptyWallHops : emptyOtherHops);
        ++emptyHopsTotal;
        ++resumes;
        retriedConnect = false;  // the next hop gets its own one-shot immediate retry
        // The record wall is the ONE signal that arms the bounded window, and the only way
        // the window ever shrinks. Ask for less next time so the peer has less to write, and
        // the record it writes is an allocation this heap can actually serve.
        // sink.total > 0 is REQUIRED, not a nicety. A satisfied bounded range comes back
        // responseComplete(), and the "is there more?" test for an unknown-length body is
        // exactly responseComplete() -- so windowing a chunked or connection-delimited
        // response would report the first window as the whole file and silently truncate it.
        // The short-range branch that continues a window is itself guarded on sink.total > 0.
        if (hitRecordWall && !windowFutile && sink.total > 0) {
          const size_t before = rangeWindow;
          if (rangeWindow == RANGE_WINDOW_MIN && !windowProved) {
            // Floored, and not one whole window ever arrived: this peer frames its records the
            // same way however little we ask for, so the window is not the lever here. Give it
            // up rather than keep shrinking into a wall it cannot move.
            rangeWindow = 0;
            windowFutile = true;
            SdDebugLog::log("HTTP", "range window abandoned at %zu — bounding the range did not help", resumeOffset);
          } else {
            rangeWindow = rangeWindow == 0 ? RANGE_WINDOW_START
                                           : (rangeWindow / 2 < RANGE_WINDOW_MIN ? RANGE_WINDOW_MIN : rangeWindow / 2);
          }
          windowWins = 0;
          if (rangeWindow != before && rangeWindow != 0) {
            SdDebugLog::log("HTTP", "range window %zu -> %zu (record wall at %zu)", before, rangeWindow, resumeOffset);
          }
        }
        // Unlike the connect-failure ladder, this DOES record the error: the hop opened and
        // hit the record wall, which is precisely what the range-restart decision needs to
        // know to refuse a pointless full restart into the same wall.
        lastHopTlsErr = http.lastTlsError();
        uint32_t backoffMs = EMPTY_HOP_WALL_DELAY_MS;
        if (!hitRecordWall) {
          const uint32_t rawBackoffMs = 500u * static_cast<uint32_t>(emptyOtherHops);
          backoffMs = rawBackoffMs > MAX_RESUME_BACKOFF_MS ? MAX_RESUME_BACKOFF_MS : rawBackoffMs;
        }
        SdDebugLog::log("HTTP", "empty hop at %zu/%zu, waiting %lums (%s %d/%d, attempt %d/%d, tlsErr=%d)",
                        resumeOffset, sink.total, (unsigned long)backoffMs, hitRecordWall ? "wall" : "empty",
                        hitRecordWall ? emptyWallHops : emptyOtherHops, emptyLimit, resumes, resumeBudget,
                        lastHopTlsErr);
        // Sliced for the same reason as the connect ladder: nothing polls input during a
        // backoff, so an unsliced delay is a window with a dead Cancel button.
        for (uint32_t slept = 0; slept < backoffMs && !(sink.cancelFlag && *sink.cancelFlag); slept += 100) {
          delay(100);
        }
        if (sink.cancelFlag && *sink.cancelFlag) return HttpDownloader::ABORTED;
        continue;
      }

      // A FRESH hop that answered 200/206 and then delivered nothing gets a small number of
      // re-issues too. This is the one shape the resumed ladder above cannot cover, because
      // it insists on resumeOffset > 0 — and it is now the most common way a font download
      // dies outright: the capture has Vollkorn_12.cpfont ending twice on `got 0 of 0
      // resumes=0`, on a URL that had opened with 15473 bytes minutes earlier.
      //
      // Re-issuing is trivially safe: nothing has been written, resumeOffset is 0, so this
      // is the identical GET the caller would make if the user pressed the retry button.
      // What it is NOT is self-limiting — no offset advances and nothing resets
      // freshRetries — so the counter is the entire bound and is never reset anywhere.
      const int freshLimit = hitRecordWall ? MAX_FRESH_RETRIES_RECORD_WALL : MAX_FRESH_RETRIES;
      if (resumeOffset == 0 && sink.downloaded == 0 && freshRetries < freshLimit &&
          !(sink.cancelFlag && *sink.cancelFlag)) {
        ++freshRetries;
        retriedConnect = false;
        SdDebugLog::log("HTTP", "first hop delivered nothing, retrying (%d/%d, tlsErr=%d)", freshRetries, freshLimit,
                        http.lastTlsError());
        for (uint32_t slept = 0; slept < EMPTY_HOP_WALL_DELAY_MS && !(sink.cancelFlag && *sink.cancelFlag);
             slept += 100) {
          delay(100);
        }
        if (sink.cancelFlag && *sink.cancelFlag) return HttpDownloader::ABORTED;
        continue;
      }
      SdDebugLog::log("HTTP",
                      "resume budget spent: got=%zu/%zu resumes=%d stalls=%d wall=%d empty=%d connectFail=%d "
                      "emptyAll=%d fresh=%d lastHop=%zu tlsErr=%d",
                      sink.downloaded, sink.total, resumes, stalledResumes, emptyWallHops, emptyOtherHops,
                      connectFailures, emptyHopsTotal, freshRetries, hopBytes, http.lastTlsError());
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
    if (sink.total > 0 && sink.downloaded < sink.total && sink.downloaded > resumeOffset && resumes < resumeBudget &&
        stalledResumes < MAX_RESUME_STALLS && !(sink.cancelFlag && *sink.cancelFlag)) {
      const size_t progressFloor = rangeWindow > 0 ? rangeWindow / 4 : MIN_RESUME_HOP_BYTES;
      stalledResumes = shortHopBytes < progressFloor ? stalledResumes + 1 : 0;
      emptyWallHops = 0;  // this hop carried bytes
      emptyOtherHops = 0;
      connectFailures = 0;
      ++resumes;
      resumeOffset = sink.downloaded;
      retriedConnect = false;
      SdDebugLog::log("HTTP", "short range, resuming at %zu/%zu (attempt %d/%d, hop=%zu stalls=%d)", resumeOffset,
                      sink.total, resumes, resumeBudget, shortHopBytes, stalledResumes);
      noteWindowWin(shortHopBytes);
      continue;
    }
    {
      // Whole request, not the final hop. transferStartMs and the *TotalMs pair are restamped
      // on every hop's first body byte, so reading them here described only the last few
      // hundred milliseconds of a transfer that had taken minutes: the 08-20 capture reports
      // a completed 739694-byte font as "elapsed=546ms rate=1354750B/s" for a download that
      // actually ran 70.5 seconds. Anyone judging download health from that line was reading
      // a number three orders of magnitude wrong, so DONE now uses the request-wide clock.
      const uint32_t totalElapsedMs = millis() - requestStartMs;
      const unsigned bytesPerSec = totalElapsedMs > 0 ? (unsigned)(sink.downloaded * 1000UL / totalElapsedMs) : 0;
      // wait= is time blocked on the socket, work= is our own per-chunk cost (SD write
      // plus the caller's progress callback). They should roughly sum to elapsed; a large
      // work= means we are the bottleneck, a large wait= with a small work= means the
      // link or the server is. On X3 a repaint shows up in wait=, not work=, because
      // requestUpdate() only posts to the render task — which then takes the SPI bus the
      // SD card shares, so the NEXT read blocks.
      SdDebugLog::log("DONE", "bytes=%zu elapsed=%lums rate=%uB/s wait=%lums work=%lums resumes=%d empty=%d fresh=%d",
                      sink.downloaded, (unsigned long)totalElapsedMs, bytesPerSec, (unsigned long)waitAllMs,
                      (unsigned long)workAllMs, resumes, emptyHopsTotal, freshRetries);
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
