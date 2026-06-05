# Work Summary — OPDS / HTTPS path (X3/X4, ESP32-C3, ~380KB RAM)

Shipped fixes + UX for the OPDS / HTTPS path, plus a documented X3-only investigation
that hit the silicon's edge. All debug instrumentation has been removed; only the
shippable changes below remain.

## 1. HTTPS "Memory error" / "Not enough memory for sync" (TLS heap) — FIXED

**Symptom:** Frequent on-screen `Memory error` (OPDS) and `Not enough memory for sync`
(KOReader), worse on X3, worst over HTTPS.

**Root cause:** Deliberate pre-handshake OOM gates firing because the mbedTLS handshake
can't get enough heap. ESP-IDF default TLS record buffers are 16384 in + 16384 out =
32KB/connection; the X509 cert-chain parse adds a small-alloc storm; the 48KB framebuffer
+ caches fragment the heap so the *largest contiguous* block drops below the gate.

**Fix:** Shrink per-connection mbedTLS heap via `custom_sdkconfig` in `platformio.ini`:
- `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` — frees record/CA buffers between records
- `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`
- `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192`, `OUT=2048` (down from 16384/16384)
- `KEEP_PEER_CERTIFICATE` off (override did not stick — force-selected by a dep; minor)

Record buffers cut 32768 → 10240 B, plus dynamic freeing during the X509 parse window.

**Build-system impact:** `custom_sdkconfig` switches Arduino from prebuilt libs to
**ESP-IDF-from-source**, which pulls unused telemetry components (`esp_rainmaker`,
`esp_insights`, ...) whose embedded certs trip a pioarduino `_embed_files.py` doubled-path
bug. Worked around with `custom_component_remove` (rainmaker/insights/provisioning stack —
all unused). Build ~40-75s warm. **Still needs all 4 envs + WiFi/OTA re-verified on
hardware before this is production-ready.**

## 2. TLS preflight gate 44KB → 24KB — FIXED

**Symptom:** After the TLS shrink, OPDS fetches with ample heap were still rejected. Logs
showed aborts at `largest=34804` (X3) and `largest=45044` (X4, under the 45056 gate by 12
bytes) while total free was 68-86KB.

**Root cause:** The 44KB contiguous gate was sized for the old 16KB+16KB record buffers.
Post-shrink the largest single TLS alloc is the ~8.2KB IN record, so 44KB over-rejected.

**Fix:** `MIN_CONTIGUOUS_HEAP_FOR_TLS` 44KB → 24KB (~3x headroom over the 8.2KB IN record).
Cleared the false rejects on both devices.

## 3. OPDS list scroll extremely slow — FIXED

**Root cause:** `render()` re-checked "is this book already on SD" for every visible row on
every redraw — up to ~12 `Storage.exists()` each, ~276 serialized SD stats per keypress.

**Fix:** Compute the on-SD marker once per feed load into `downloadedCache`
(`std::vector<uint8_t>`, parallel to `entries`), refreshed at the single `fetchFeed()` choke
point. `render()` reads the cache. Per-keypress SD ops ~276 → 0.

## 4. OPDS "Loading..." with no progress / no escape — FIXED

**Constraint:** The render task is event-driven (no timer), and `fetchFeed()` blocks the
loop thread inside the handshake/transfer; feeds usually carry no Content-Length.

**Fix:**
- Phase labels: `Connecting...` (And-Wait, before the blocking handshake), `Downloading
  N KB/MB (Ns)` with elapsed seconds from the progress callback (throttled 8KB), `Parsing...`.
- **Cancel:** Back is polled inside the per-chunk progress callback; the downloader checks a
  `cancelFetch` flag before each socket read and returns `ABORTED`. User backs out instead
  of rebooting. (Works while bytes trickle; a fully dead read still waits the socket timeout.)
- New i18n `STR_PARSING`, `STR_LOADING_CANCELLED`; reused `STR_CONNECTING`.

## 5. Delete-confirmation for OPDS + KOReader servers — NEW

Both server-editor Delete rows now launch the shared `ConfirmationActivity` (Cancel/Confirm)
before `removeServer()`. Prevents a mis-press from destroying a configured server. No new
strings (reused `STR_DELETE_SERVER`/`STR_CANCEL`/`STR_CONFIRM`).

## 6. WiFi modem-sleep off during transfers — NEW

`HttpDownloader::runGet` now holds `WIFI_PS_NONE` for the whole transfer (RAII `NoWifiSleep`,
restored on every exit), mirroring `OtaUpdater`. Standard practice for sustained transfers.

## X3-only slow/stalling HTTPS feed — INVESTIGATED, NOT FIXED (hardware edge)

**Symptom:** On X3 only, large HTTPS OPDS feeds (e.g. 50-entry bookmarks, ~38KB) crawl
8KB-at-a-time with multi-second stalls; transfers of 60-230s; sometimes only a reboot escapes.
X4 fetches the same feed from the same server over the same AP in ~600ms.

**Method:** Added temporary SD instrumentation (now removed) — per-chunk read/write split,
`maxReadGap`, and on any >1s read stall a snapshot of `MALLOC_CAP_INTERNAL` free/largest and
RSSI.

**Findings (proven, not guessed):**
- Stall is 100% in the socket read (`writeMs ~100ms`); a single `esp_http_client_read()`
  blocked up to ~55s = TCP RTO back-off from dropped frames.
- **RSSI steady -52..-58 during every stall** → not RF / antenna.
- X4 fast on the same server → not the server.
- **`intLargest` (internal SRAM largest block) craters to ~2KB while `intFree` ~6KB during
  the transfer.** WiFi dynamic RX buffers (~1.6KB) come from internal SRAM; at ~2KB largest
  it can't allocate one → frames dropped at the MAC → retransmit → RTO stalls.

**Things tried that did NOT fix it (all reverted):**
- Widen TCP window 5760 → 16384 + window scaling, deeper recvmbox/BA-win — RTO still hit;
  X3 loses enough frames that even fast-retransmit can't recover.
- Raise static WiFi RX buffers (8 → 12 → 20), disable `MBEDTLS_DYNAMIC_BUFFER` to stop
  fragmentation — internal `intLargest` still craters to ~2KB; these RAM-hungry knobs also
  eat the very internal SRAM that is the bottleneck.

**Conclusion:** X3's internal SRAM is too tight for a large HTTPS feed transfer concurrent
with the 48KB framebuffer + fonts + app. No PSRAM on C3 to escape to. **Confidence to fully
fix in firmware: low (~20-30%).** Practical workarounds: serve the OPDS feed over plain
**http** (removes the TLS internal-RAM cost — most effective), or use a smaller server page
size for big feeds on X3.

## Files touched (vs prior `feat-dictionary`)
- `platformio.ini` — `custom_sdkconfig` (mbedTLS shrink), `custom_component_remove`
- `src/activities/browser/OpdsBookBrowserActivity.{cpp,h}` — 24KB gate, scroll cache,
  load progress + elapsed, Back-to-cancel
- `src/activities/settings/OpdsSettingsActivity.cpp` — delete-confirmation
- `src/activities/settings/KOReaderSettingsActivity.cpp` — delete-confirmation
- `src/activities/reader/KOReaderSyncActivity.cpp` — instrumentation removed
- `lib/KOReaderSync/KOReaderSyncClient.cpp` — instrumentation removed
- `src/network/HttpDownloader.cpp` — `NoWifiSleep` (modem-sleep off), cancel plumbing
- `lib/I18n/translations/english.yaml` — `STR_PARSING`, `STR_LOADING_CANCELLED`

## Open follow-ups
- Verify the source-build switch across all 4 envs + WiFi/OTA on hardware before production.
- Properly drop `KEEP_PEER_CERTIFICATE` (find the selector) for a small extra heap win.
- X3 + big HTTPS feeds: use an http mirror or smaller page size; not a firmware fix.
