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
