# Change Summary — OPDS robustness, downloads, file sizes

Branch: `feat-dictionary`. Builds clean (`pio run`). Device verification (X3 + X4) pending where noted.

---

## 1. OPDS feed/download — real error on screen + in logs

**Goal:** "Failed to fetch feed" gave no cause. Surface the real reason on the UI and serial/SD log.

**`HttpDownloader.cpp/.h`:** `downloadToFile` gained an optional `std::string* errorDetail`.
Every failure path fills a short reason via a stack-buffer helper (`setDetail`, no hot-path
`std::string`): `"HTTP <code>"`, `"connect failed: <esp_err>"`, `"redirect failed"`,
`"out of memory (heap=...)"`, `"read error after N bytes"`, `"incomplete: N/M bytes"`,
`"empty response"`, `"cannot open SD file"`.

**`OpdsBookBrowserActivity.cpp`:** feed and parse failures now `LOG_ERR` + append the detail to
the on-screen message (`"Failed to fetch feed: HTTP 401"`); error line truncated to viewport
width (X3 narrower). Download failures likewise show the cause.

---

## 2. X3 download out-of-memory fix

**Symptom (X3 logs):** book download failed with `ESP_ERR_HTTP_CONNECT`, then
`OOM: 2048 byte read buffer`. A 50-entry feed left only ~39 KB free; the HTTPS handshake
needs ~40 KB contiguous, and the read buffer was allocated *after* the TLS connection (when
heap is fragmented to a few KB).

**Fixes:**
- `OpdsBookBrowserActivity::downloadBook`: copy the book by value, then **free the `entries`
  vector before connecting** (reclaims feed RAM for the TLS handshake); reload the feed after
  a successful download and restore the selection. Logs `heap` + `largest free block` at start.
- `HttpDownloader::runGet`: **allocate the read buffer first**, before opening the connection,
  while 70 KB+ is free and contiguous — removes the post-connect OOM.

**Note:** X3 is at the edge for HTTPS downloads (live mbedtls ~65 KB + 48 KB framebuffer). A
mid-stream `read error` can still occur under pressure; the real further lever is shrinking
mbedtls record buffers, which needs a custom ESP-IDF build (not the Arduino prebuilt libs).

---

## 3. Download progress on screen

**`HttpDownloader.cpp`:** progress callback now fires even when the server sends no
`Content-Length` (chunked / redirected CDN), with `total==0` meaning "size unknown".

**`OpdsBookBrowserActivity.cpp` (DOWNLOADING render):** known size → percentage bar (as before);
unknown size → bytes received, scaled `KB → MB → GB`. Redraws throttled to every 64 KB
(e-ink refresh is slow). Applies to X3 and X4 (shared path, orientation-aware width).

**`FontDownloadActivity.cpp` (regression guard):** keeps the manifest-provided size when the
server omits `Content-Length` (`if (total > 0) fileTotal_ = total`).

---

## 4. Per-server download folders + finished-books subfolder

**Goal:** contain each OPDS server's books in its own folder; move finished books into that
folder's `read/` subfolder (e.g. server "readeck" → `/readeck/…`, finished → `/readeck/read/…`).

**`OpdsBookBrowserActivity.cpp`:** `serverFolder(name)` → `/<sanitized server name>` (empty
name = card root, legacy behavior). Downloads go to `/<server>/<file>.epub` (folder created on
demand). The "already on device" marker checks `<folder>/`, `<folder>/read/`, **plus** legacy
`/` and `/read/` so older downloads still show the `*`.

**`EpubReaderActivity.cpp`:** the finished-books move is now **relative to the book's own
folder** — `READ_SUBFOLDER = "read"`, destination `<parentDir>/read/`. Root books still go to
`/read/` (back-compat). `isInReadFolder` rewritten to test whether the immediate parent dir is
named `read` (prevents re-moving at any depth).

The on-device file browser already navigates subfolders, so foldered books are openable.

---

## 5. OPDS alphabetical sort — now per-server (was global)

**`OpdsServerStore.h` + `JsonSettingsIO.cpp`:** `OpdsServer` gained `bool sortAlphabetical`
(persisted as `sort_az`, defaults true so existing servers keep sorting).

**`OpdsSettingsActivity.cpp`:** the server editor now has a **Sort A-Z** toggle (Name / URL /
Username / Password / Sort A-Z / Delete) with an ON/OFF value.

**`OpdsServerListActivity.cpp`:** removed the global A-Z virtual toggle row.
`OpdsBookBrowserActivity.cpp` sorts on `server.sortAlphabetical`.
Removed the now-dead global `opdsSortAlphabetical` (`CrossPointSettings.h`, `SettingsList.h`).

---

## 6. File browser — show file size

**Goal:** show each file's size on one line beside the extension, in a small font.

**`FsHelpers.cpp/.h`:** extracted the natural-sort comparator as `naturalFileLess` (single
source; `sortFileList` calls it) so richer entries can sort by name without duplicating logic.

**`FileBrowserActivity.h/.cpp`:** `files` is now `vector<FileEntry{name, size}>` (+4 bytes/entry).
Size is read from the already-fetched directory entry during `loadFiles()` — **no extra SD
I/O** — and kept beside the name so sorting can't desync the two. Trailing value shows
`"<ext>  <size>"`; `formatFileSize` renders MB with 1 decimal (GB past 1 GB); directories blank.

**Themes (`BaseTheme` / `LyraTheme` / `RoundedRaffTheme` `drawList`):** added a defaulted
`valueSmallFont = false` param so the file browser draws the value column in the small font.
Every other list is byte-identical (flag off by default).

**Cost:** +4 bytes/file RAM, ~negligible flash; no extra I/O, no measurable CPU/heat.

---

## 7. OPDS X3 pagination OOM — freeze + read error

**Symptom (X3 logs):** paging to the 2nd feed page (`?limit=50&offset=50`) failed with
`read error after N bytes, heap=13104`, after a ~215 s UI freeze. The previous page's 50
entries (~33 KB) were still held while the new feed's TLS connection came up; entries + the
~54 KB mbedtls connection starved the heap, and the 16 KB record buffer couldn't be allocated
mid-stream.

**Fixes (`OpdsBookBrowserActivity.cpp`):**
- `fetchFeed`: **free the `entries` vector before the new feed's TLS connection** (mirrors
  `downloadBook` from §2). The parser fully repopulates `entries` after the connection closes;
  ERROR paths don't read the list. This removed the multi-minute freeze (page-2 now fails fast
  instead of hanging).
- **TLS heap preflight** in both `fetchFeed` and `downloadBook`: if the largest contiguous free
  block < `MIN_CONTIGUOUS_HEAP_FOR_TLS` (44 KB), bail to the ERROR state with `STR_MEMORY_ERROR`
  instead of attempting a connect that OOMs mid-stream. RETRY reloads with more headroom (entries
  already freed); BACK navigates out.

**Note:** these stop the crash/freeze but can't make HTTPS page-2 *fit* on X3 — the prebuilt
Arduino mbedtls bakes in `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384` (two 16 KB record buffers)
and the `tls_dyn_buf_strategy` knob isn't in the esp_http_client header this build compiles
against. **Reliable X3 OPDS = serve the catalog over plain `http://`** on a trusted network
(frees the ~54 KB TLS cost; the firmware already supports http for local servers). The full
firmware lever — shrinking the mbedtls record buffer — still needs a custom ESP-IDF build.

---

## 8. Reader "Indexing" popup — real progress bar

**Goal:** a long chapter index showed a static "Indexing" popup with no movement and looked
like a hang.

**Fixes:** threaded a percent callback (`std::function<void(int)>`, was `void()`) through
`ChapterHtmlSlimParser` → `Section::createSectionFile` → `EpubReaderActivity`. The parser tracks
`bytesRead / file.size()` in the XML read loop and reports percent; `EpubReaderActivity` captures
the popup `Rect` from `drawPopup` and fills the existing `fillPopupProgress` bar.

**Cost / throttle:** each bar redraw is a full ~637 ms FAST e-ink refresh (see
`BmpViewerActivity`), so updates are throttled on **elapsed time** (≥ 750 ms) — bounded overhead
that scales with parse duration, not chapter size. The final-buffer redraw is skipped (the page
render replaces the popup). Chapters under 10 KB still skip the popup entirely. Silent
next-chapter prefetch passes no callback, so it draws nothing. No new heap, no new task.

---

## 9. WiFi-connected indicator in the header

**Goal:** show a WiFi icon beside the battery when connected.

**`BaseTheme.cpp/.h`:** new shared `drawWifiBars()` draws small rising signal bars (4 bars,
primitives — no asset; `drawIcon` can't scale the 32×32 `WifiIcon` down to the ~12 px header).
Called from `drawBatteryRight`, positioned left of the battery group, so all three themes
(Base/RoundedRaff/Lyra, which all route headers through `drawBatteryRight`) get it from one edit.
Drawn **only when `WiFi.status() == WL_CONNECTED`**; self-clears its footprint first so a stale
indicator can't linger where a theme's header clear doesn't reach that far left (RoundedRaff).

**Behavior note:** WiFi is torn down on exit from every network activity, so this is correctly
blank on Home/menus and appears on the header-using network screens where the link is up
(KOReader sync, font download, Calibre, network-mode select). Read-only `WiFi.status()` query —
no radio start, no heap, persistence/power design untouched.

---

## 10. Skip the WiFi list when already connected

**Goal:** if WiFi is still connected, don't re-scan and re-pick — reuse the live connection.

**`WifiSelectionActivity.cpp` (`onEnter`):** if `WiFi.status() == WL_CONNECTED` with a valid IP,
populate `selectedSSID`/`connectedIP` from the current link, set state `CONNECTED`, and
`onComplete(true)` immediately — skipping the scan/selection list. Safe from `onEnter` because
`finish()` → `popActivity()` only sets a deferred pending Pop (handled next loop), so the activity
isn't deleted mid-entry.

**RAM design respected:** read-only reuse; does not make WiFi persistent. After the normal
teardown (disconnect + `silentRestart`) or auto-sleep (`WIFI_OFF`), WiFi is down and the next
entry falls through to the usual scan/list. The session itself is unchanged — it lasts only as
long as the network activity is active, bounded by the inactivity auto-sleep (default 10 min).

---

## 11. Bookmark sync — Last-Writer-Wins by Lamport version

**Symptoms (multi-device, X3 + X4):** stale tombstones deleted freshly-created bookmarks
(`remote 1 + local 3 -> total 1`); a re-added bookmark wouldn't propagate to the other device;
and deletes wouldn't propagate. Root cause: the merge had no way to order create-vs-delete, so
it picked a fixed winner — delete-wins lost re-adds, add-wins lost deletes. The C3 has no
battery-backed RTC, so wall-clock timestamps aren't usable (`Bookmark.timestamp` was always 0).

**Fix (`BookmarkStore.cpp/.h`):** order events with a per-book **Lamport logical clock** instead
of a real clock.
- `Bookmark.timestamp` is repurposed as **`version`** — same 4-byte on-disk slot, so the bookmark
  file format is unchanged and pre-existing bookmarks read back as version 0 (lowest priority).
- `Tombstone` gains a `version`; tombstone file format **v1 → v2** with auto-migration (legacy
  tombstones load as version 0).
- A per-book `lamportCounter`: `nextVersion()` (`++counter`) stamps every local add/delete so a
  delete always outranks the bookmark it replaces; `observeVersion()` raises the counter past
  every version seen from a remote during merge, so the next local edit outranks everything seen
  (causal ordering). The counter is rebuilt on load as `max(version)` over all stored entries —
  exact, because the highest-version entry always survives a merge (no separately persisted value).
- **`mergeFrom` rewritten as LWW:** group local + remote bookmarks and tombstones by spot; per
  spot the highest `version` wins (bookmark → alive, tombstone → dead); a version tie keeps the
  bookmark (never silently lose one). Winners are tracked by pointer into the existing vectors —
  no `Bookmark` copies (~124 B each), so RAM stays low even at the 1024 cap. Persists only on a
  real change.
- Sync blob JSON: bookmarks/tombstones now carry `version`; parse falls back to the legacy
  `timestamp` key for older blobs.

**Result:** delete propagates (tombstone newer than bookmark wins everywhere) **and** re-add
resurrects (new bookmark newer than tombstone wins everywhere), converging with no clock. The
sequential use-one-device-then-sync workflow orders correctly; truly concurrent edits between
syncs fall to the bookmark-wins tie-break. Mixed-firmware fleet during rollout converges once
both devices update (old firmware treats `version` as 0).

**Not addressed (separate, deferred):** independently-created (non-synced) bookmarks at the same
reading spot still get different keys on different-height screens (page-derived `progress` /
`paragraphIndex`), so they won't dedupe cross-device. Synced bookmarks share identical keys.

---

## 12. KOReader sync — auto-return to reader after upload

**Goal:** after a progress upload, return to the reader on its own instead of waiting for a manual
Back press (the apply-remote path already auto-returned).

**`KOReaderSyncActivity.cpp/.h`:** entering `UPLOAD_COMPLETE` records `uploadCompleteAt = millis()`;
`loop()` returns to the reader once `UPLOAD_COMPLETE_AUTO_RETURN_MS` (3 s) elapses, leaving time to
read the confirmation. Manual Back still returns immediately. Scope is the upload path only;
`SYNC_FAILED` / `NO_CREDENTIALS` stay manual so their messages can be read. `loop()` runs every
main-loop cycle (no keypress needed), the same cadence the WiFi connect-timeout relies on.

---

## 13. EPUB reader — "return here" bookmark on chapter jump

**Goal:** when the user jumps to another chapter, auto-drop a bookmark at the page they left so they
can get back quickly, shown with a distinct (hollow) icon, and consumed once used.

**Create (`EpubReaderActivity.cpp`):** the `SELECT_CHAPTER` handler drops a bookmark at the current
page before switching, only when the target chapter differs and the page isn't already bookmarked
(won't clobber a manual one). `addBookmark(bool returnMark)` passes the flag through.

**Model (`BookmarkStore.h/.cpp`):** `Bookmark` gains a `returnMark` flag. Persisted to the bookmark
file — format **v5 → v6** adds a per-record flag byte (`SNIPPET_VERSION=5`, `RETURN_MARK_VERSION=6`),
`isKnownVersion()` centralizes the version checks, old files auto-migrate (flag reads false). So the
hollow mark survives closing/reopening the book.

**Device-only, never synced:** `serializeToJson` skips return marks; all delete paths
(`removeBookmarkForPage` / `removeBookmarkAt` / `clearAll` / `removeReturnMarkAt`) skip the tombstone
for a return mark — nothing to propagate. The sync summary counts (`KOReaderSyncActivity::syncBookmarks`)
exclude return marks via `countSyncable()` so the totals match what actually syncs.

**Distinct icon:** status bar (`BaseTheme::drawStatusBar`) draws a hollow tab vs the solid normal
bookmark; bookmark list shows a hollow icon — new `UIIcon::BookmarkReturn` + 32×32 `bookmarkReturn.h`
(outline derived from the solid glyph), wired in `LyraTheme` (the only theme that renders list icons).
Driven by `BOOKMARKS.isReturnMarkForPage()` / `bm.returnMark`.

**Consume on reopen:** opening the return mark from the bookmark list calls `removeReturnMarkAt()`
(matches the merge key, only removes a return mark) — the one-shot aid is dropped once used.

---

## 14. KOReader sync — fix X4 return-to-reader (hang + ghosting)

**Symptom:** on X4, returning from the sync screen showed "Loading" then hung / faded white; X3 only
showed a grainy transition. Serial showed the silent reboot fired but X4 stuck.

**Hang — WiFi left on across the soft reset (`main.cpp`):** callers only did `WiFi.disconnect(false)`
(radio stays on); `ESP.restart()` with the modem alive wedged X4's heavy reader-boot. New
`wifiPowerDownForReboot()` (`WiFi.disconnect(true)` + `WIFI_OFF`, mirroring the proven deep-sleep
teardown) runs at the top of both `silentRestart()` and `silentRestartToReader()`.
`KOReaderSyncActivity::onExit` simplified to rely on it; `returnToReader()` guarded fire-once
(`returning`) since the auto-return check is level-triggered.

**Ghosting — seamless boot skips the panel clear (`GfxRenderer` + `main.cpp`):** the first reader
paint was `FAST_REFRESH`, ghosting the pre-reboot "Progress found" frame. New one-shot
`forceCleanRefreshNextPaint()` upgrades the next `displayBuffer()` to `HALF_REFRESH` (clears stale
residue, no FULL black/white flash), triggered at boot only for `Silent` resume to `READER`.

---

## 15. Return mark on "Go to %" + distinct notification text

**"Go to %" drops a return mark too (`EpubReaderActivity.cpp`):** the `GO_TO_PERCENT` result
callback now mirrors `SELECT_CHAPTER` — before `jumpToPercent()` it drops a `returnMark` bookmark at
the current page, guarded by `targetPercent != initialPercent` (actually moving) and
`!hasBookmarkForPage(...)` (don't clobber a manual one). Captures `initialPercent` in the lambda;
`addBookmark(returnMark=true)` is called before the jump because `jumpToPercent()` resets `section`.

**Distinct toast text (`EpubReaderActivity.cpp/.h` + i18n):** new `bookmarkMessageReturn` flag, set
from the `returnMark` arg in `addBookmark()` and cleared on the removal path. The reader popup now
picks one of three strings: `STR_BOOKMARK_REMOVED` / new `STR_RETURN_MARK_ADDED` ("Return mark
added.") / `STR_BOOKMARK_ADDED`. So a manual bookmark says "Bookmark added." while an auto chapter/
percent jump says "Return mark added." English string added; other languages fall back until
translated.
