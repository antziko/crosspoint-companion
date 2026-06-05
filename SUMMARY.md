# Work Summary — TLS heap + OPDS browser

Branch origin: `fix/tls-heap` (merged into `feat-dictionary`).

Three related fixes around the OPDS / HTTPS path on the X3/X4 (ESP32-C3, ~380KB RAM).

## 1. HTTPS "Memory error" / "Not enough memory for sync" (TLS heap)

**Symptom:** Frequent on-screen `Memory error` (OPDS) and `Not enough memory for sync`
(KOReader), worse on X3, worst over HTTPS.

**Root cause:** All three messages are deliberate pre-handshake OOM gates firing because
the mbedTLS handshake can't get enough heap.
- ESP-IDF default TLS record buffers are 16384 B in + 16384 B out = 32KB per connection.
- Cert-chain (X509) parse makes many small allocs (~48KB aggregate on multi-cert chains).
- The 48KB framebuffer + caches fragment the heap, so the *largest contiguous* block drops
  below the gate even when total free looks OK. Hence intermittent, worse on X3 (less free).
- The gates are each correct for their failure mode: OPDS checks largest-contiguous-block
  (44KB) for the record buffer; KOSync checks total-free (55KB) for the small-alloc storm.

**Fix:** Shrink per-connection mbedTLS heap via `custom_sdkconfig` in `platformio.ini`:
- `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` — frees record/CA buffers between records (biggest lever)
- `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`
- `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=8192`, `OUT=2048` (down from 16384/16384)
- (`KEEP_PEER_CERTIFICATE` override did not stick — force-selected by a dependency; minor)

TLS record buffers cut 32768 → 10240 B, plus dynamic freeing during the X509 parse window.

**Build-system impact:** `custom_sdkconfig` switches the Arduino framework from prebuilt
libs to **ESP-IDF-from-source**. That pulls unused telemetry components (`esp_rainmaker`,
`esp_insights`, ...) whose embedded certs trip a pioarduino `_embed_files.py` doubled-path
bug. Worked around with `custom_component_remove` (rainmaker/insights/provisioning stack —
all unused by CrossPoint). Build is ~40-75s warm (one-time toolchain download), NOT the
20-40 min initially feared.

**Status:** Builds clean, fits flash (~80.7% app partition). Heap win is runtime-only, so it
**must be verified on hardware** — see instrumentation below. Source-build switch needs all 4
envs + WiFi/OTA re-verified before this is considered production-ready.

**Instrumentation (temporary, for the X3 which is USB-locked / no serial):** both handshake
paths log `free` and `largest contiguous block` to the SD file `/opds_debug.log` under the
`TLSMEM` tag. Read the card after triggering an HTTPS OPDS fetch and a KOReader sync; compare
`largest` vs the 44KB gate and `free` vs the 55KB gate. Remove once measured.

## 2. OPDS list scroll extremely slow

**Root cause:** `OpdsBookBrowserActivity::render()` re-checked "is this book already on SD"
for every visible row on every redraw — and each check did up to ~12 `Storage.exists()` SD
lookups. With 23 rows that is ~276 serialized SD stat operations **per cursor keypress**.

**Fix:** Compute the on-SD marker once per feed load into a cached `downloadedCache`
(`std::vector<uint8_t>`, parallel to `entries`), refreshed at the single `fetchFeed()` choke
point (covers navigation and the post-download reload). `render()` reads the cache.
Per-keypress SD ops: ~276 → 0.

## 3. OPDS "Loading..." with no progress

**Constraint:** The render task is event-driven (no timer tick), and `fetchFeed()` blocks the
loop thread inside the TLS handshake — so nothing can animate during the handshake, and feeds
usually carry no Content-Length (no true %).

**Fix:** Phase labels + byte counter:
- `Connecting...` painted (And-Wait) before the blocking handshake, so the stall is labeled
  rather than a frozen `Loading...`.
- `Downloading... N KB/MB` from the download progress callback (was `nullptr`), throttled to
  every 8KB to avoid flooding the slow e-ink refresh.
- `Parsing...` before the parse loop.
- New i18n `STR_PARSING`; reused existing `STR_CONNECTING`.

## Files touched
- `platformio.ini` — custom_sdkconfig (mbedTLS), custom_component_remove
- `src/activities/browser/OpdsBookBrowserActivity.{cpp,h}` — scroll cache + load progress + TLSMEM log
- `src/activities/reader/KOReaderSyncActivity.cpp` — KOSync TLSMEM SD log
- `lib/KOReaderSync/KOReaderSyncClient.cpp` — KOSync handshake free/largest log
- `lib/I18n/translations/english.yaml` — `STR_PARSING`
- `.gitignore` — ignore source-build byproducts

## Open follow-ups
- Flash to X3, read `/opds_debug.log` `TLSMEM` lines, confirm the gates clear. Then decide on
  productionizing the source-build switch (CI cost + 4-env/hardware re-verify).
- Remove the `TLSMEM` instrumentation once measured.
- Properly drop `KEEP_PEER_CERTIFICATE` (find the selector) for a small extra heap win.
