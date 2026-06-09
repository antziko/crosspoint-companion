# Change Summary — `feat-dictionary` (X3/X4, ESP32-C3, ~380KB RAM)

Builds clean (`pio run`). Device verification (X3 + X4) pending where noted.

Two tracks of work:
- **Part A (§1–24):** feature/robustness changes across OPDS, downloads, reader, bookmarks, sync, file browser, sleep.
- **Part B (§25–30 + appendix):** the later OPDS / HTTPS deep-dive that shrank the mbedTLS heap — the "real lever" §2 and §7 below could only flag as future work. All debug instrumentation from that investigation has been removed; only shippable changes remain.

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
**→ This lever was later implemented — see §25 (TLS heap shrink) and §26 (gate retune).**

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
**→ That custom build was later done — see §25; the 44 KB gate here was retuned to 24 KB in §26.**

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

---

## 16. OPDS Servers — hold-Select to duplicate a server

**Goal:** quickly clone an existing OPDS server entry (same host, tweak path/creds) without
re-typing all fields.

**Gesture:** hold Confirm ≥ 1 s on a real server row in settings mode → `ConfirmationActivity`
"Duplicate this server?" (server name as body) → confirm → new entry appended with name
`<original> (copy)` (or `<URL> (copy)` when unnamed); all fields copied; new row selected.
Cancel → no change. Tap (release) still opens the editor as before.

**`OpdsServerStore.h`:** added `static constexpr size_t maxServers()` so the list activity can
gate the gesture when the 8-server cap is full (no prompt when full; `addServer` is a second guard).

**`OpdsServerListActivity.cpp/.h`:**
- `longPressFired` member + release-swallow guard (matches `HomeActivity` pattern).
- `loop()` restructured: hold branch checked on `isPressed` + `getHeldTime() >= 1000`; normal
  tap moved to `wasReleased` so press-down doesn't race with the hold path.
- `duplicateSelectedServer()`: copies `OpdsServer` struct by value, appends `tr(STR_OPDS_COPY_SUFFIX)`,
  shows `ConfirmationActivity`, calls `OPDS_STORE.addServer()` on confirm.
- Guard: `!pickerMode`, `selectedIndex < serverCount`, `getCount() < maxServers()`.

**i18n (`english.yaml`):** `STR_OPDS_DUPLICATE_SERVER` ("Duplicate this server?"),
`STR_OPDS_COPY_SUFFIX` (" (copy)"). Other languages fall back to English until translated.

---

## 17. Bookmark list — page X/Y subtitle

**Goal:** show page position in the bookmark list so users can gauge how far into the chapter a bookmark is.

**`BookmarkStore.h/.cpp`:** `Bookmark` gains `uint16_t chapterCurrentPage` and `uint16_t chapterPageCount` (0 = unknown). On-disk format **v6 → v7**; v6 files read both as 0 and auto-migrate on next save. `addBookmark` captures `currentPage` / `pageCount` at creation time — no epub load in the list.

**`EpubReaderBookmarksActivity.cpp`:** subtitle shows `"<pct>% — <page>/<total> — <chapter>"` when page count is known; falls back to `"<pct>% — <chapter>"` otherwise. Page counts are a creation-time snapshot and may drift if render settings repaginate; chapter title stays stable.

**Sync JSON:** both fields carried; `parseFromJson` defaults to 0 when absent — older/other-firmware peers stay compatible. Merge key, tombstones, and Lamport versioning unchanged.

Port of the upstream `feat-dictionary` subtitle; upstream's version read fields (`computedChapterProgress`/`PageCount`) and an epub member removed in this fork, so it was dropped at merge. This re-implements the intent without re-adding the stale model.

---

## 18. Home screen — hold-Confirm to remove a recent book

**Goal:** let users remove a book from the recent-books row on the home screen without navigating to the full recent-books list.

**Gesture:** hold Confirm ≥ 1 s while a recent-book tile is selected → `ConfirmationActivity` "Remove from Recent Books?" → confirm → removed and cover tile dropped; selector clamped. Cancel → no change. A `longPressFired` guard swallows the release so the hold can't also open the book.

**`HomeActivity.cpp/.h`:** `longPressFired` member + release-swallow guard (same pattern as `RecentBooksActivity`). `promptRemoveRecentBook` calls existing `RecentBooksStore::removeByPath` (persists), refreshes the recents vector, and clears the cached cover tile. No new strings or store changes — reuses `ConfirmationActivity` and `STR_REMOVE_FROM_RECENTS`.

---

## 19. File browser — hold Home to toggle show hidden files

**Goal:** toggle "Show Hidden Files" in-place from the file browser without navigating to Settings.

**Gesture:** hold Back ≥ 1 s while at the SD-card root (where the button hint already reads **HOME**) → `SETTINGS.showHiddenFiles` flipped, persisted via `SETTINGS.saveToFile()`, file list reloaded immediately. Hold again to toggle back. Selector clamped if the list shrinks.

**No collision:** the existing hold-Back-to-root gesture requires `basepath != "/"` — our gesture is gated on `basepath == "/"`, so they never overlap. At root, hold-Back was previously a no-op.

**`FileBrowserActivity.h/.cpp`:** `hiddenToggleFired` guard member (same swallow-release pattern as hold-Confirm-delete). Toggle fires on `isPressed(Back) && getHeldTime() >= GO_HOME_MS && basepath == "/"`, checked before the existing hold-Back branch. Short Back tap at root still goes home; hold Back in subfolders still jumps to root — both unchanged.

**Persistence:** writes `SETTINGS.saveToFile()` — same call as SettingsActivity; the toggle is by definition a value change so no extra guard needed. The Settings-menu "Show Hidden Files" toggle reflects the same flag.

---

## 20. Sleep screen — full refresh before custom wallpaper

**Goal:** eliminate ghosting from the prior screen under sleep-folder wallpapers.

**`SleepActivity.cpp` (`renderBitmapSleepScreen`):** the pre-draw ghost-wipe at line 252 changed from `HALF_REFRESH` to `FULL_REFRESH`. The post-draw present (line 268) remains `HALF_REFRESH` to avoid a second disruptive flash over the final image.

`FULL_REFRESH` drives the full black/white waveform from a white baseline, completely clearing the prior screen before the wallpaper is drawn. Scope is `renderBitmapSleepScreen` only (the shared path for both `CUSTOM` and `COVER_CUSTOM` modes); default, blank, and quick-resume sleep variants are untouched.

---

## 21. Reader Options (per-book) — apply live + single-source I/O

**Symptoms (page menu → Reader Options):** changes did not take effect on the book, and after editing the screen exited all the way to Home.

**Exit-to-Home fix (`EpubReaderActivity.cpp`):** the `READER_OPTIONS` result callback now sets `ignoreBackUntilRelease = true`. Reader Options consumes the Back **press**, but its **release** bled through into the reader's `onGoHome()` (`wasReleased(Back)`), navigating out. Swallowing the trailing release keeps the user in the book.

**Apply-on-return fix:** the same callback takes a `RenderLock`, calls `sdFontSystem.ensureLoaded(renderer)`, then `section.reset()` so the page reflows with the just-saved override (built-in font size already changed the font ID; SD fonts now reload — see §22).

**Single-source persistence (`ReaderSettingsIO.{h,cpp}`, new):** `reader_settings.bin` load/write was duplicated in `EpubReaderActivity` and the options editor — drift risk on field order / file version. Extracted to `namespace ReaderSettingsIO { load(); write(); }` (constants `READER_SETTINGS_FILE_VERSION = 1`, `READER_SETTINGS_FILENAME`). Both the reader (seed/load on open) and `ReaderOptionsActivity::persistAndApply` now call the shared functions; the writer and reader can no longer diverge.

---

## 22. Per-book font size for SD (CJK) fonts

**Symptom:** in Reader Options the per-book **font size** worked for built-in Latin fonts but had no effect for SD-card fonts (Japanese/Chinese/Korean). Root cause: `SdCardFontSystem::fontSizeEnumFromSettings()` read the **global** `SETTINGS.fontSize`, the single SD font was only ever loaded at that global size, and the reader never asked it to reload — so the page font ID never changed and the section cache never invalidated.

**Key fact:** the SD font ID already encodes point size (`SdCardFontManager::computeFontId(hash, family, pointSize)`). Once the font is actually reloaded at the override size, `getFontId()` returns a different ID and the section layout cache invalidates **automatically** — no `SECTION_FILE_VERSION` bump.

**Fixes:**
- `CrossPointSettings.{h,cpp}`: new override-aware `getReaderFontSize()` (returns `readerOverride.fontSize` when active, else global), mirroring the existing `getReaderFontId`/alignment getters.
- `SdCardFontSystem.cpp`: `fontSizeEnumFromSettings()` reads `getReaderFontSize()`. This makes both `begin()` (boot/global) and `ensureLoaded()` honor the override when active and fall back to global otherwise.
- `EpubReaderActivity.cpp`: call `sdFontSystem.ensureLoaded(renderer)` at three points — after `setReaderOverride` in **onEnter** (match this book's size before first layout), in the **Reader Options return** callback (reload at the new size, see §21), and after `clearReaderOverride` in **onExit** (restore the global SD size for Home/library).

**Decision (confirmed):** only one SD font is loaded at one size, shared by the page and the dictionary popup's CJK glyph fallback. Page size wins — the dictionary's primary (Latin) text keeps its own global dict font/size, and there is **no SD reload on dictionary open/close** (no lookup slowdown). CJK glyphs inside the popup follow the current per-book page size (accepted limitation of the single-SD-font model).

---

## 23. Reader Options → Font Family — full picker (built-in + SD)

**Goal:** Reader Options' font-family item should offer the **full** font list (built-in + SD-card fonts), like the global *Reader > Reader Font Family* screen, instead of cycling the two built-ins only.

**Refactor (`FontSelectionActivity.{h,cpp}`):** the shared picker no longer reads/writes global `SETTINGS` for its selection. Constructor now takes the current selection (`uint8_t currentBuiltinFamily`, `std::string currentSdFamilyName`); `handleSelection()` builds a `FontSelectionResult` and returns it via `setResult()` + `finish()`. The "Selected" marker uses a new `currentSelectionIndex()` helper.

**Result type (`ActivityResult.h`):** new `struct FontSelectionResult { bool isBuiltin; uint8_t builtinIndex; std::string sdFamilyName; }` added to `ResultVariant`.

**Reader Options (`ReaderOptionsActivity.cpp`):** the Confirm handler special-cases the font-family item to launch the picker via `startActivityForResult`; the callback applies the `FontSelectionResult` to **this book's** `localOverride` (builtin index, or `strncpy` the SD family name) and `persistAndApply()`. The old built-in-only cycle case was removed.

**Global parity (`SettingsActivity.cpp`):** the global Reader Font Family launch now passes `SETTINGS.fontFamily` / `SETTINGS.sdFontFamilyName` and the callback applies the returned result to global settings, then `saveToFile()` + `rebuildSettingsLists()` — behavior unchanged from before, just routed through the new result path.

**Override-aware family (`CrossPointSettings.{h,cpp}` + `SdCardFontSystem.cpp`):** new `getReaderSdFontFamilyName()` (override-aware). `ensureLoaded()` resolves the wanted family from it; a per-book SD font that fails to load only clears the *global* selection when no override is active (`clearWantedFamily` lambda) — a missing per-book font can never wipe the user's global font choice.

---

## 24. cppcheck cleanups (`pio check -e default`)

- **`OpdsBookBrowserActivity.cpp`:** the entry-dedup loop in the `add` lambda replaced with `std::any_of` (`<algorithm>` already included) — clearer intent, same behavior.
- **`KOReaderServerListActivity.cpp`:** removed a dead `if (itemCount > 0)` guard (`getItemCount()` is always ≥ 1); added a comment, navigation lambdas unchanged.

---
---

# Part B — OPDS / HTTPS path deep-dive (TLS heap)

Shipped fixes + UX for the OPDS / HTTPS path, plus a documented X3-only investigation
that hit the silicon's edge. This is the follow-on that implemented the "shrink mbedTLS record
buffer" lever §2 and §7 could only flag. All debug instrumentation has been removed; only the
shippable changes below remain.

## 25. HTTPS "Memory error" / "Not enough memory for sync" (TLS heap) — FIXED

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

## 26. TLS preflight gate 44KB → 24KB — FIXED

**Symptom:** After the TLS shrink (§25), OPDS fetches with ample heap were still rejected. Logs
showed aborts at `largest=34804` (X3) and `largest=45044` (X4, under the 45056 gate by 12
bytes) while total free was 68-86KB.

**Root cause:** The 44KB contiguous gate (§7's `MIN_CONTIGUOUS_HEAP_FOR_TLS`) was sized for the
old 16KB+16KB record buffers. Post-shrink the largest single TLS alloc is the ~8.2KB IN record,
so 44KB over-rejected.

**Fix:** `MIN_CONTIGUOUS_HEAP_FOR_TLS` 44KB → 24KB (~3x headroom over the 8.2KB IN record).
Cleared the false rejects on both devices.

## 27. OPDS list scroll extremely slow — FIXED

**Root cause:** `render()` re-checked "is this book already on SD" for every visible row on
every redraw — up to ~12 `Storage.exists()` each, ~276 serialized SD stats per keypress.

**Fix:** Compute the on-SD marker once per feed load into `downloadedCache`
(`std::vector<uint8_t>`, parallel to `entries`), refreshed at the single `fetchFeed()` choke
point. `render()` reads the cache. Per-keypress SD ops ~276 → 0.

## 28. OPDS "Loading..." with no progress / no escape — FIXED

**Constraint:** The render task is event-driven (no timer), and `fetchFeed()` blocks the
loop thread inside the handshake/transfer; feeds usually carry no Content-Length.

**Fix:**
- Phase labels: `Connecting...` (And-Wait, before the blocking handshake), `Downloading
  N KB/MB (Ns)` with elapsed seconds from the progress callback (throttled 8KB), `Parsing...`.
- **Cancel:** Back is polled inside the per-chunk progress callback; the downloader checks a
  `cancelFetch` flag before each socket read and returns `ABORTED`. User backs out instead
  of rebooting. (Works while bytes trickle; a fully dead read still waits the socket timeout.)
- New i18n `STR_PARSING`, `STR_LOADING_CANCELLED`; reused `STR_CONNECTING`.

## 29. Delete-confirmation for OPDS + KOReader servers — NEW

Both server-editor Delete rows now launch the shared `ConfirmationActivity` (Cancel/Confirm)
before `removeServer()`. Prevents a mis-press from destroying a configured server. No new
strings (reused `STR_DELETE_SERVER`/`STR_CANCEL`/`STR_CONFIRM`).

## 30. WiFi modem-sleep off during transfers — NEW

`HttpDownloader::runGet` now holds `WIFI_PS_NONE` for the whole transfer (RAII `NoWifiSleep`,
restored on every exit), mirroring `OtaUpdater`. Standard practice for sustained transfers.

## Appendix — X3-only slow/stalling HTTPS feed: INVESTIGATED, NOT FIXED (hardware edge)

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
- **Suppress e-ink panel refresh during the transfer** — tested on-device (X3) with a
  per-chunk probe (read-loop `readMs`/`betweenMs` + `intLargest`/`intFree` sampling) under an
  A/B build flag. **Ruled out:** refresh-ON `betweenMs` stayed ≤40ms (panel not blocking the
  read loop), and download stalls began at chunk #0 before any throttled repaint, so the
  panel was not the trigger. Feeds were already fast in both arms (the TLS-heap work, not
  refresh). The remaining large-download stalls are the same `intLargest`→~2KB / TCP-RTO
  edge above, triggered by transfer *size* (~4.5MB export.epub) at strong RSSI (-42..-46) —
  not the display. Probe was throwaway, fully reverted.

**Conclusion:** X3's internal SRAM is too tight for a large HTTPS feed transfer concurrent
with the 48KB framebuffer + fonts + app. No PSRAM on C3 to escape to. **Confidence to fully
fix in firmware: low (~20-30%).** Practical workarounds: serve the OPDS feed over plain
**http** (removes the TLS internal-RAM cost — most effective), or use a smaller server page
size for big feeds on X3.

---

## Files touched by Part B (vs prior `feat-dictionary`)
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
- Verify the source-build switch (§25) across all 4 envs + WiFi/OTA on hardware before production.
- Properly drop `KEEP_PEER_CERTIFICATE` (find the selector) for a small extra heap win.
- X3 + big HTTPS feeds: use an http mirror or smaller page size; not a firmware fix.

---

# Part C — KOReader Sync HTTPS fixes (X3 heap + UX)

## 31. KOReader HTTPS auth from settings — better error + workaround UX

**Symptom (X3):** Authenticating a KOReader sync server with HTTPS always showed "not enough memory for sync" on X3. X4 with HTTPS or any HTTP worked fine.

**Root cause:** `MIN_HEAP_FOR_TLS = 55000` in `KOReaderSyncClient.cpp`. After WiFi connects in settings context, X3 has only ~53KB free — below the 55KB threshold. The threshold is correct: Cloudflare 3-cert chains exhaust ~48KB during mbedTLS X509 parse; with only 53KB available the session starves to 2600 bytes min-free before failing with `MBEDTLS_ERR_X509_ALLOC_FAILED`. The threshold cannot be safely lowered for Cloudflare users.

**Investigated and reverted:**
- Lowering threshold to 40KB: guard passed but TLS still failed (`NETWORK_ERROR`).
- Increasing `HTTP_BUF_SIZE` 2048→4096: still failed, also triggered crash reporter via `silentRestartToSettings()` (the "crash" was intentional but noisy).

**Shipped fix:** Updated `LOW_MEMORY` error string from generic "please retry" to `"Not enough memory for HTTPS sync. Open a book and sync from the reader instead."` — which is the actual valid workaround (reader context frees the epub before TLS, see §32).

**Files:** `lib/KOReaderSync/KOReaderSyncClient.cpp` (error string + comment).

---

## 32. KOReader HTTPS sync from reader — "fetch ok / upload failed"

**Symptom:** Syncing from within the reader with HTTPS: bookmark fetch succeeded but upload failed.

**Root cause:** `syncBookmarks()` called `ensureEpubLoaded()` then attempted two sequential TLS connections (GET bookmarks, PUT bookmarks) with the epub still in memory. The epub holds ~30KB of parsed data. After the first TLS handshake + JSON allocations, remaining free heap was below 55KB, and the second TLS (PUT) was blocked by the `MIN_HEAP_FOR_TLS` gate. Similarly, `performUpload()` called `updateProgress()` TLS with the epub loaded.

**Fix (`KOReaderSyncActivity.cpp`):**
- `syncBookmarks()`: extract title and author from epub, then `epub.reset()` **before** any TLS call. `loadForBook()` keys on path CRC (already available as `epubPath` member), not title/author — these are only display metadata, safe to capture first. After sync completes `performSync()` reloads the epub for the reader.
- `performUpload()`: added `epub.reset()` before the `updateProgress()` TLS call.

**Result:** both TLS calls in bookmark sync and progress upload now have ~30KB more free heap, pushing well above the 55KB threshold.

**Files:** `src/activities/reader/KOReaderSyncActivity.cpp`.

---

## 33. KOReader settings — "Set as Active" (no WiFi needed)

**Goal:** Switch the active sync server without authenticating (no WiFi required). Previously the only path to change `activeIndex` was via the Authenticate row, which needs WiFi and credentials.

**Change (`KOReaderSettingsActivity.cpp`):** Added `ROW_SET_ACTIVE = 5`, shifting `ROW_AUTHENTICATE` to 6, `ROW_DELETE` to 7, and `BASE_ITEMS_EXISTING` to 7. Only one server can be active; the bullet (•) value marker on `ROW_SET_ACTIVE` shows which server is currently active. Selecting the row on the already-active server is a no-op; selecting it on any other server calls `KOREADER_STORE.setActiveIndex()` + `saveToFile()` immediately — no WiFi, no restart.

**i18n (`lib/I18n/translations/english.yaml`):** Added `STR_SET_AS_ACTIVE: "Set as Active"`. Other languages fall back to English until translated.

**Files:** `src/activities/settings/KOReaderSettingsActivity.cpp`, `lib/I18n/translations/english.yaml`.

---

## 34. KOReader sync header shows active server name

**Goal:** "KOReader Sync" header was generic when multiple servers exist; users couldn't confirm which server was active.

**Change (`KOReaderSyncActivity.cpp` `render()`):** Reads `KOREADER_STORE.getServer(activeIndex)`. If the server has a non-empty name, header becomes `"KOReader Sync - <name>"` (stack buffer `char[72]`, `snprintf`). Falls back to plain `"KOReader Sync"` when name is empty or server pointer is null.

**Files:** `src/activities/reader/KOReaderSyncActivity.cpp`.

---
---

# Part D — Reading Stats feature + Vega home theme (checkpoint)

New cross-cutting feature: per-book + global reading-time tracking (X3 RTC-dated, X4 undated),
two stats screens, and a new home-screen theme ("Vega") that surfaces it on the home screen.
Full design/status tracked in `plan.md`. Phase 1+2 last build-verified via `pio run -e default`
-> SUCCESS; Phase 2.5 (Vega) **verified on hardware (X3/X4) by the user — closed**.

## 35. Reading Stats core — per-book + global tracking, X3-dated / X4-undated

**`BookReadingStats` (`src/activities/reader/BookReadingStats.{h,cpp}`, new):** per-book
`stats.bin` — binary, versioned, reject-on-mismatch ("fresh start" on schema bump by design, no
migration code). v3 = 19 bytes: `totalReadingSeconds`/`unattributedSeconds` (u32 each),
`avgSecondsPerForwardPage`/`paceSampleCount` (u16 each), `lastReadDayIndex` (u32),
`lastReadHour`/`lastReadMinute` (u8 each).

**`ReadingTimeHistory` (new):** weekly/monthly/yearly time buckets + a 730-day heatmap bitset,
keyed by a flat day-index (`readingHistoryDayIndex`/`readingHistoryDateFromDayIndex`/
`readingHistoryDayOfWeek` — proleptic-calendar counter anchored at 2000-01-01 = day 0, leap-year
aware via `isLeapYear`/`kDaysInMonth`; weekday is pure `dayIndex % 7`). X3-only (needs RTC).

**`GlobalReadingStats` (new):** cumulative totals + an embedded `ReadingTimeHistory`, persisted
to `/.crosspoint/global_stats.bin`.

**Wiring (`EpubReaderActivity.cpp`):** pace sampling (forward-page seconds, outlier-filtered —
see `8b34c3d2`) and `recordReadingSession()` called once per session from `onExit` (debounced
per CLAUDE.md, not per page-turn); one `halClock.getDate()`/`getTime()` pair, gated by RTC
availability (`dated`). Raw RTC values are stored with **no UTC offset applied** — same
convention the status-bar clock uses; the offset is applied at *display* time (see §38).

**`HalClock` gains date support (`lib/hal/HalClock.{h,cpp}`, `HalGPIO.h`):** `getDate()`/
`formatDate()` (DS3231 day-of-week/date/month/year registers — new `DS3231_DOW_REG`), mirroring
the existing `getTime()`/`formatTime()` cache + UTC-offset + rollover pattern, incl. the
`>104`-corrupted-value clamp. New `STR_DATE_FORMAT`/`STR_DATE_FMT_0..3` settings +
`StatusBarSettingsActivity` picker.

## 36. Reading Stats UI — per-book and global screens

**`BookStatsActivity` (new, reader menu -> "Book Stats"):** total time, dated/undated split,
estimated time remaining (progress-percent extrapolation from `avgSecondsPerForwardPage`),
Timeline/Heatmap tabs.

**`ReadingStatsActivity` (new, Home menu -> "Reading Stats", shown only when
`totalReadingSeconds > 0`):** same tab layout, sourced from `GlobalReadingStats`.

**Wiring:** `ActivityManager` (`HomeMenuItem::READING_STATS`, `goToReadingStats()`),
`HomeActivity` (conditional menu entry), new `Chart` `UIIcon` + `chart.h` glyph (wired into
`BaseTheme`/`LyraTheme`'s `iconForName`), 14 new i18n strings (`STR_READING_STATS`,
`STR_BOOK_STATS`, `STR_STATS_*`).

## 37. Vega — new home-screen theme: hero book + "next 3" + icon menu

**Goal (user spec, `plan.md`):** one current/most-recent book front-and-center (cover + title +
progress bar + duration + chapter + "Last read on ..."), a compact "next 3" row below it, and a
horizontal icon-only bottom menu — replacing the generic recent-books grid.

**`VegaTheme.{h,cpp}` (new, `src/components/themes/vega/`):** `class VegaTheme : public LyraTheme`
(same derivation `Lyra3CoversTheme` uses — Lyra's chrome for free). Overrides
`drawRecentBookCover` (hero card + cached `loadHeroDetails()` self-contained EPUB I/O inside the
existing `coverRendered` snapshot gate, "next 3" row, selection highlights) and `drawButtonMenu`
(bottom-anchored horizontal icon row + centred label + filled-highlight selection — mcrosson's
`GfxRenderer` has no `drawIconInverted`). `VegaMetrics::values` derives `homeCoverTileHeight`
from shared layout constants so metrics and draw geometry can't drift; `homeRecentBooksCount = 4`.

**Hero data sourcing:** progress %/chapter derive on demand from the existing `progress.bin` +
`epub.calculateProgress`/`getTocItem` (metadata-only `epub.load(false, true)`, the same
lightweight call `loadRecentCovers` already makes for thumbnails). New
`EpubReaderUtils::Progress`/`loadProgress` shared helper mirrors `EpubReaderActivity::onEnter`'s
read-side `progress.bin` parsing (4-or-6-byte format). "Last read on ..." sources
`BookReadingStats::load()`.

**Wiring:** `CrossPointSettings::UI_THEME::VEGA = 4`, `SettingsList.h` theme picker, `UITheme.cpp`
`setTheme()` case. New `STR_CONTINUE_READING`/`STR_HOME_LAST_READ_FORMAT`/`STR_NO_OPEN_BOOK`/
`STR_THEME_VEGA` i18n strings.

## 38. Vega fixes — cover darkening, OOB crop, progress-bar label, title overflow, UTC

Five issues found and fixed while building out and refining the hero card:

- **Cover darkening on "next 3":** thumbnails were drawn downscaled from a larger cached size,
  and `GfxRenderer::drawBitmap`'s nearest-neighbour downscale collapses pre-dithered 1-bit pixels
  via OR-only-dark BW compositing (a 2-into-1 collapse of ~50%-dithered source art renders ~75%
  black — visibly darker than the source). Fix: `VegaMetrics` draws "next 3" thumbnails at
  **native** `homeCoverHeight` — exactly the cached-thumbnail generation size
  (`UITheme::getCoverThumbPath`/`Epub::generateThumbBmp`) — landing `fitScale` at `1.0` (no
  downscale, no darkening). 140x226 native, `homeCoverTileHeight` 544, cover-buffer ~31.9 KB.

- **Negative-crop OOB read/write in `GfxRenderer::drawBitmap`** (`lib/GfxRenderer/GfxRenderer.cpp`,
  pre-existing — also present in shipped `Lyra3CoversTheme`/`SleepActivity`, not Vega-only): when
  a tile is proportionally wider/taller than its source bitmap, callers' `crop = 1 -
  tileRatio/sourceRatio` goes negative, driving `cropPixX`/`cropPixY` negative and walking
  `outputRow[]` out of its `malloc`'d bounds (under- *and* over-read). Fixed at the shared call
  site — `std::max(0.0f, cropX/cropY)` clamp ("no crop" instead of OOB) — closes the hole for all
  three callers (Vega, Lyra3Covers, SleepActivity) in one edit.

- **Hero progress-bar label tracks the fill position:** "`NN% - <duration>`" combined onto a
  single line, right-aligned over the bar's current fill edge (`fillEdgeX`, computed once and
  shared with the fill draw so they can't drift apart) — like a tooltip following a slider thumb,
  clamped to `[textX, textX+textW]` so it stays fully visible near 0%/100%.

- **Title can no longer push the detail block past the cover:** `detailBlockH` is now summed
  from the *actual* optional elements present (progress/bar/duration/chapter/last-read), and
  `titleMaxLines = min(staticCeiling, (coverH - gap - detailBlockH) / titleLineH)` — guarantees
  `title + gap + details <= coverH`. Title font `UI_12_FONT_ID -> UI_10_FONT_ID` (more chars/line).

- **"Last read on ..." now respects the user's UTC offset:** `lastReadDayIndex/Hour/Minute` are
  stored as raw RTC reads with no offset applied (matching the status-bar clock's convention);
  `formatLastRead` (`VegaTheme.cpp:52`) applies `SETTINGS.clockUtcOffsetQ` at *display* time with
  the same offset+rollover arithmetic as `HalClock::formatTime`/`formatDate` — including the
  `>104` corrupted-value clamp — shifting the day-index by ±1 on a midnight crossing and
  re-deriving date/weekday via the proleptic-calendar helpers from §35 (leap-year safe by
  construction, since ±1 on a flat day-index always lands on the adjacent calendar day). Verified
  end-to-end across the full `[-12:00,+14:00]` offset range with concrete numeric traces.

## Files touched by Part D
- New: `src/activities/reader/{BookReadingStats,GlobalReadingStats,ReadingTimeHistory,
  BookStatsActivity,ReadingStatsActivity}.{h,cpp}`, `src/components/themes/vega/VegaTheme.{h,cpp}`,
  `src/components/icons/chart.h`, `plan.md`
- Modified: `lib/hal/HalClock.{h,cpp}`, `lib/hal/HalGPIO.h`, `lib/GfxRenderer/GfxRenderer.cpp`,
  `lib/Epub/Epub/BookMetadataCache.cpp` (debug aid: human-readable `.crosspoint` cache-folder
  label files — `<hash-dir>--<title>-by-<author>.txt`, empty, filename-only), `src/activities/
  reader/{EpubReaderActivity,EpubReaderMenuActivity,EpubReaderUtils}.{h,cpp}`,
  `src/activities/home/HomeActivity.{h,cpp}`, `src/activities/ActivityManager.{h,cpp}`,
  `src/components/themes/{BaseTheme,UITheme}.{h,cpp}`, `src/components/themes/lyra/LyraTheme.cpp`,
  `src/activities/settings/StatusBarSettingsActivity.cpp`, `src/CrossPointSettings.h`,
  `src/SettingsList.h`, `lib/I18n/translations/*.yaml` (English authored; others auto-fallback
  via `gen_i18n.py`)

## Open follow-ups
- ~~Build + on-device verification (X3 + X4) for Phase 2.5~~ — done, verified by user.
- Phase 3 (KOReader stats sync) — paused, needs a re-plan against the v3 `BookReadingStats`
  schema (the existing Phase-3 sketch in `plan.md` references the dropped v2 shape).

---

## Files touched by Part C (vs prior commits)
- `lib/KOReaderSync/KOReaderSyncClient.cpp` — improved LOW_MEMORY error string + comment
- `src/activities/reader/KOReaderSyncActivity.cpp` — epub.reset() before TLS in syncBookmarks/performUpload; active server name in header
- `src/activities/settings/KOReaderSettingsActivity.cpp` — Set as Active row (ROW_SET_ACTIVE=5), shifted ROW_AUTHENTICATE/DELETE
- `lib/I18n/translations/english.yaml` — `STR_SET_AS_ACTIVE`

---
---

# Part E — Hold-to-rotate display orientation on browse/list screens

## 39. `SETTINGS.displayOrientation` — scoped to 4 screens, never ambient

**Goal:** let users rotate Browse Files / Recent Books / OPDS Browser / Reading Stats independently
of the reader's orientation — e.g. landscape for wider list columns — **without** that bleeding
into Home, Settings, or File Transfer (which must always stay Portrait).

**Design correction (caught mid-implementation):** the first pass made `displayOrientation` a
global ambient default — applied at boot, restored by the reader/sleep activities on exit. That
leaked non-Portrait orientation into Home/Settings/File Transfer whenever the user navigated there
from a rotated screen. **Fixed** by making each of the four screens **self-manage** orientation:
apply `SETTINGS.displayOrientation` in `onEnter`, force-reset to `Portrait` in `onExit` — the same
per-activity pattern `EpubReaderActivity`/`TxtReaderActivity`/`SleepActivity` already use for the
*reader's* orientation, just not shared as ambient state. This guarantees correctness regardless of
navigation order; `main.cpp`, `EpubReaderActivity`, `TxtReaderActivity`, `SleepActivity`,
`HomeActivity` are untouched (verified zero diff vs HEAD).

**New setting (`CrossPointSettings.h` / `SettingsList.h` / `english.yaml`):**
`uint8_t displayOrientation = PORTRAIT`, registered as `SettingInfo::Enum` under
`STR_CAT_DISPLAY` → "Display Orientation" (`STR_DISPLAY_ORIENTATION`), auto-persisted via the
generic settings-list JSON I/O.

**New helpers (`ReaderUtils.h`)** — generalize the reader's existing hold-to-rotate so the four
list screens can reuse it without duplicating gesture-timing logic:
- `enum class SideNavAction { NONE, STEP, ROTATE }`
- `resolveSideNavAction(input, button)`: when `SETTINGS.sideLongPressButtonBehavior !=
  ORIENTATION_CHANGE`, returns `STEP` on press (snappy single-step nav, unchanged feel). When the
  gesture *is* enabled, switches to release-based detection — measuring hold duration before
  deciding, mirroring `detectPageTurn`'s `usePress` branch — `ROTATE` if held > `SKIP_HOLD_MS`
  (700 ms), else `STEP`. A single press can't safely fire both list-nav and rotate, hence the
  switch to release-based timing only when the gesture is opted into.
- `cycleDisplayOrientation(renderer, step)`: cycles `SETTINGS.displayOrientation` ±1 (wrapping),
  applies immediately via `applyOrientation`, persists with `saveToFile()` — mirrors the reader's
  hold-to-rotate persistence exactly.

**The four screens (`FileBrowserActivity`, `RecentBooksActivity`, `OpdsBookBrowserActivity`,
`ReadingStatsActivity`):** identical `onEnter`/`onExit` pair (`applyOrientation` /
`setOrientation(Portrait)`). In `loop()`, physical side Up/Down are routed through
`resolveSideNavAction`: `STEP` → existing single-step list-cursor/scroll move, `ROTATE` →
`cycleDisplayOrientation(±1)`, `NONE` → no-op. Front Left/Right keep their prior behavior
unchanged (continuous-repeat page-jump in the browsers; reorder in `RecentBooksActivity`, which
uniquely reserves Left/Right for that and was left untouched). Replacing the old continuous-scroll
side-button handlers with single-step removes the page-jump on those two buttons — a deliberate
trade so holding them can be measured for the rotate gesture; this matches how the reader already
treats Up/Down once any long-press behavior is active.

**Sign convention note (`ReadingStatsActivity`):** Up→`cycleDisplayOrientation(+1)`,
Down→`cycleDisplayOrientation(-1)` — opposite of the list-cursor STEP direction on that axis, but
consistent with the reader's existing hold-to-rotate convention (hold "previous"/Up rotates `+1`,
"next"/Down rotates `-1`).

**Verification:** `pio run` → SUCCESS (RAM 31.4%, Flash ~81%); `pio run -t unit-tests` → 79/79
pass. Hardware checklist (user, pending): hold-rotate on all 4 screens × 4 orientations; confirm
Home/Settings/File Transfer stay Portrait in practice; confirm `displayOrientation` persists
across reboot.

## Files touched by Part E
- `src/activities/reader/ReaderUtils.h` — `SideNavAction`, `resolveSideNavAction`,
  `cycleDisplayOrientation`
- `src/activities/home/{FileBrowserActivity,RecentBooksActivity}.cpp` — onEnter/onExit + side-button rotate gesture
- `src/activities/browser/OpdsBookBrowserActivity.cpp` — onEnter/onExit + side-button rotate gesture
- `src/activities/reader/ReadingStatsActivity.cpp` — onEnter/onExit + side-button rotate gesture
- `src/CrossPointSettings.h`, `src/SettingsList.h` — `displayOrientation` setting
- `lib/I18n/translations/english.yaml` — `STR_DISPLAY_ORIENTATION`

---

## 40. Fix — hold-to-rotate dead on Reading Stats Heatmap tab

**Symptom:** on `ReadingStatsActivity`, the §39 hold-to-rotate gesture (side Up/Down long-press)
worked on the Timeline tab but did nothing on the Heatmap tab.

**Cause (`ReadingStatsActivity.cpp:132`):** the `resolveSideNavAction`/`cycleDisplayOrientation`
switches were nested inside `if (selectedTab == Tab::Timeline && !timelineRows.empty())` — guarding
scroll-state setup (`contentRect`/`maxOffset`) that ROTATE doesn't need. On the Heatmap tab (or an
empty Timeline) `resolveSideNavAction` was never even called, so the long-press was never measured
and never produced `ROTATE`.

**Fix:** moved the two `resolveSideNavAction` switches out of that tab check (mirroring how the
other three §39 screens call it unconditionally in `loop()`). `STEP` now checks a `canScrollTimeline`
flag before touching `scrollOffset`/`maxOffset` (computed only when that flag is true); `ROTATE`
fires unconditionally on both tabs, same as `FileBrowserActivity`/`RecentBooksActivity`/
`OpdsBookBrowserActivity`.

**Verification:** `pio run` clean. Hardware checklist (user, pending): hold side Up/Down on the
Heatmap tab with `sideLongPressButtonBehavior == ORIENTATION_CHANGE` set — display should rotate;
Timeline-tab scroll behavior unchanged.

---
---

# Part F — Heatmap intensity levels + reading-session local-date fix

## 41. Heatmap 4-level intensity (None/Light/Moderate/Heavy) + UTC-offset session-date bug — FIXED

**Goal (user spec):** upgrade the reading-stats heatmap from a binary "did you read that day"
presence bitmap to a 4-shade intensity grid: white = untracked or no reading, light gray = ≤30min,
dark gray = ≤1h, black = >1h.

**`ReadingTimeHistory.{h,cpp}`:** `heatmapBits` repacked 1 bit/day (730-day presence bitmap) →
2 bits/day (`enum class HeatmapLevel { None, Light, Moderate, Heavy }`,
`HEATMAP_BYTES = (HEATMAP_DAYS*2+7)/8`). New `classifyHeatmapLevel(seconds)` buckets by
`HEATMAP_LIGHT_MAX_SECONDS` (30min) / `HEATMAP_MODERATE_MAX_SECONDS` (1h), inclusive lower tier.
New persisted `heatmapAnchorSeconds` running per-day total — `recordDay` reclassifies the anchor
slot from the *accumulated* total on every call (a calendar day gets one `recordDay()` per
session, possibly several) rather than overwriting from a single session's length, which would
misclassify e.g. two 20-min sessions as "Light" instead of the correct 40-min "Moderate". Backdated
sessions (clock-adjustment edge case) can only raise, never lower, an already-finalized slot — no
running total is kept for past days. New `getHeatmapLevel(daysAgo)`; `isHeatmapDaySet()` kept as a
thin `!= None` wrapper for callers that only care about presence. `HISTORY_FILE_VERSION` 1 → 2
(new field forces a fresh start by design — reject-on-mismatch, same convention as
`BookReadingStats`, no migration code).

**Render (`BookStatsActivity.cpp` / `ReadingStatsActivity.cpp`):** grid fill switched from
binary solid-black/dithered-gray to an exhaustive `switch` on `HeatmapLevel` →
`Color::Black` (solid `fillRect`) / `DarkGray` / `LightGray` (`fillRectDither`), `None` left
blank. Untracked days and tracked-but-no-reading days now render identically (both white) —
collapsing a distinction the old binary model drew that the new 4-level one doesn't need.

**Tests (`test/reading-time-history/`, new gtest suite, 7 cases):** threshold-boundary
inclusivity, same-day accumulation crossing a level, day-rollover finalize+reset of the running
total, multi-day shift across packed-byte boundaries (2 bits/day → 4 days/byte), backdated
upgrade-only semantics, persistence round-trip of `heatmapAnchorSeconds`, presence-wrapper
agreement. Registered in `test/CMakeLists.txt`. While bringing the suite up, fixed a stub bug:
`HalFile::read` stubbed as `uint8_t*` vs the real `void*` (`lib/hal/HalStorage.h:88`) —
`Serialization::readString`'s `char*` call site failed `-fpermissive` conversion.

**Bug found + fixed while verifying — reading sessions dated by raw RTC, not local calendar
day:** `EpubReaderActivity::onExit` called `halClock.getDate()`/`getTime()` raw — no
`SETTINGS.clockUtcOffsetQ` applied — before handing the date to `recordReadingSession` →
`ReadingTimeHistory::recordDay`. A session started just after local midnight could still read
as the RTC's previous (UTC-ish) day, so it accumulated into *yesterday's* heatmap/weekly/
monthly/yearly buckets instead of starting today's — exactly the "still shows Sunday, not
Monday" symptom that surfaced this. This was already known and worked around for *display only*
(§38's `formatLastRead` re-applies the offset to the stored "Last read on..." stamp) but the
underlying stats storage itself was never corrected.

- New `HalClock::getLocalDateTime(utcOffsetQuarterHoursBiased, ...)` (`lib/hal/HalClock.{h,cpp}`)
  factors the offset+rollover arithmetic shared by `formatDate`/`formatTime`/
  `VegaTheme::formatLastRead` into one tested implementation returning fields (not a string),
  for callers that need to *bucket* data by local calendar day rather than just display it.
- `EpubReaderActivity::onExit` now calls it with `SETTINGS.clockUtcOffsetQ`, so
  `year/month/day/dayOfWeek/hour/minute` — and therefore every weekly/monthly/yearly/heatmap
  bucket and `lastReadDayIndex/Hour/Minute` — are stored as local-calendar values going forward.
- `VegaTheme::formatLastRead` simplified: the stored values are now already local, so its §38
  offset-reapplication was removed (kept, it would have double-shifted the displayed stamp).

**Note:** `pio run` not yet re-run after the `HalClock`/`EpubReaderActivity`/`VegaTheme` edits —
interrupted before the full firmware build could confirm they compile (host gtest only exercises
`ReadingTimeHistory.cpp`). Flag for build + on-device verification: heatmap shading on X3/X4,
"Last read on..." correctness across non-zero `clockUtcOffsetQ`, heap.

## Files touched by Part F
- `src/activities/reader/ReadingTimeHistory.{h,cpp}` — 2-bit intensity heatmap,
  `heatmapAnchorSeconds`, `HISTORY_FILE_VERSION` 1→2
- `src/activities/reader/{BookStatsActivity,ReadingStatsActivity}.cpp` — 4-shade grid render
- `lib/hal/HalClock.{h,cpp}` — new `getLocalDateTime()`
- `src/activities/reader/EpubReaderActivity.cpp` — session date capture via
  `getLocalDateTime(SETTINGS.clockUtcOffsetQ, ...)`
- `src/components/themes/vega/VegaTheme.cpp` — `formatLastRead` simplified (no longer
  re-applies the UTC offset)
- New: `test/reading-time-history/` (gtest suite + stubs + `CMakeLists.txt`), registered in
  `test/CMakeLists.txt`

## Open follow-ups
- Build (`pio run`) + on-device verification: heatmap shading on hardware, "Last read" stamp
  with a non-default `clockUtcOffsetQ`, heap
- Existing on-disk per-book/global history files reset to fresh on first load
  (`HISTORY_FILE_VERSION` bump) — by design (matches `BookReadingStats` convention); worth a
  release-note line so users aren't surprised their heatmap history resets once

---

# Part G — Delete-cache confirm, OPDS status-line fix, TLS-buffer investigation

## 42. Reader "Delete Book Cache" — confirmation dialog

**Symptom:** The reader menu's "Delete Book Cache" wiped the cache immediately on select —
one mis-press destroyed the book's parsed/rendered cache (forcing a slow full re-parse).

**Fix (`EpubReaderActivity.cpp`, `DELETE_CACHE` action):** Wrap the destructive block in
`startActivityForResult(ConfirmationActivity(...))`. On `isCancelled`, return without touching
the cache; on confirm, run the existing clear-and-save-progress block (`section.reset()` →
`clearCache()` → `setupCacheDir()` → `saveProgress()`), then `onGoHome()`. Mirrors the
delete-confirmation pattern from §29 / §5's bookmark delete.

**i18n:** `STR_CONFIRM_DELETE_CACHE: "Delete book cache?"`.

**Files:** `src/activities/reader/EpubReaderActivity.cpp`, `lib/I18n/translations/english.yaml`.

---

## 43. OPDS browser — duplicate/garbled status lines after download

**Symptom:** After a book download finished (or was cancelled with Back), the screen briefly
stacked mismatched status lines — e.g. the fixed `Downloading...` label + a stale byte counter
(`24 KB / 10.1 KB`) overlaid on the reload's `Connecting.../Parsing...`.

**Root cause:** `downloadBook()` calls `fetchFeed()` to reload the list after a download, but
left `state` at `DOWNLOADING`. `fetchFeed()` paints its own phase text into `statusMessage`
(rendered as one centered line in `LOADING`), while the DOWNLOADING render branch *also* drew
the fixed label + the stale `downloadProgress`/`downloadTotal` from the just-finished transfer.

**Fix (`OpdsBookBrowserActivity.cpp`):** In both the `ABORTED` and `OK` branches, before the
`fetchFeed()` reload, drop to `state = LOADING`, set `statusMessage = STR_LOADING`, and zero
`downloadProgress`/`downloadTotal` — mirroring the `navigateToEntry`/`navigateBack` reset
pattern. One clean status line.

**Files:** `src/activities/browser/OpdsBookBrowserActivity.cpp`.

---

## 44. X3 HTTPS TLS-buffer tuning — INVESTIGATED, reverted to known-good (no change shipped)

**Context:** Chasing slow OPDS *downloads* on X3 over HTTPS, an A/B experiment raised
`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` 8192→16384 (and briefly turned `DYNAMIC_BUFFER` off with
static 16384/16384). The off-variant immediately broke KOSync upload-from-reader
(`LOW_MEMORY` / "fetch ok / upload failed") — exactly the failure §31/§32 + the
`MIN_HEAP_FOR_TLS=55000` budget exist to prevent (`KOReaderSyncClient.cpp:26-35`).

**Git evidence (`git log -p platformio.ini`):** `IN_CONTENT_LEN` has been touched by exactly
**one** commit ever — `944cc0a3`, which set it to **8192**. `16384` was never committed; it
existed only as the throwaway experiment. So the "suddenly fast OPDS *browsing* on X3+HTTPS"
state = the shipped **8192** config (§25) + the **24KB** preflight gate (§26), *not* 16384.
Slow *downloads* are the separate internal-SRAM/TCP-RTO hardware edge (Appendix) — independent
of record-buffer size, not fixable by this knob.

**Outcome:** `platformio.ini` reverted byte-for-byte to HEAD
(`DYNAMIC_BUFFER=y`, `ASYMMETRIC=y`, `IN=8192`, `OUT=2048`). **No TLS change shipped** — the
known-good config was already correct. Downloads over HTTPS on X3 stay slow by hardware limit;
workaround remains plain `http://` for the catalog (Appendix).

**Files:** none (investigation only; working tree restored to HEAD).

---

# Part H — X3 HTTPS troubleshooting instrumentation + KOSync bookmark-PUT fix + OPDS download-name UX

## 45. SD-based HTTPS troubleshooting trace (`SdDebugLog`) wired into the OPDS/KOSync paths — NEW

**Goal:** Capture on-device evidence for the X3 HTTPS slowness (Appendix/§44) without a serial
cable — `SdDebugLog::setEnabled()`-gated, writes to SD, 4MB ring (`MAX_LOG_BYTES`).

**Wiring (`onEnter`/`onExit` of each activity that drives an HTTPS call):**
- `OpdsBookBrowserActivity` — covers feed-fetch + book-download (`runGet`).
- `KOReaderSyncActivity` — covers `authenticate`/`getProgress`/`updateProgress`/
  `getBookmarks`/`updateBookmarks`.
- `KOReaderAuthActivity` — covers `authenticate` from the settings flow.

**Instrumentation added:**
- `HttpDownloader::runGet` — `CONNECT`/`STALL`/`XFER`/`DONE`/`HTTP` lines: heap, largest
  contiguous block (8-bit + internal), RSSI, byte counts, elapsed/rate. `STALL` fires on any
  per-chunk read gap > 1s (`STALL_LOG_THRESHOLD_MS`).
- `KOReaderSyncClient` — new `beginTrace()`/`endTrace()` + `httpEventHandler` hooks logging
  `KOSYNC`/`STALL` lines (TLS-connect duration, inter-event stall gaps, request/response size +
  elapsed) for `AUTH`, `PROGRESS_GET/PUT`, `BOOKMARKS_GET/PUT` — mirrors `runGet`'s shape so
  the `perform()`-based path and the streaming-GET path produce directly comparable evidence.

**Structural fix required mid-implementation:** `SdDebugLog` lived in `src/util/`, but
`KOReaderSyncClient.cpp` is under `lib/KOReaderSync/` — PlatformIO's Library Dependency Finder
gives `lib/` libraries an isolated include path that does **not** see `src/` (confirmed by the
build error `fatal error: util/SdDebugLog.h: No such file or directory`). Moved it to
`lib/SdDebugLog/` (matching the `Logging`/`Memory` cross-cutting-utility convention) via
`git mv`, history preserved; updated all 5 include sites from `"util/SdDebugLog.h"` to
`<SdDebugLog.h>`.

**Files:** `lib/SdDebugLog/{SdDebugLog.h,cpp}` (moved from `src/util/`), `lib/KOReaderSync/KOReaderSyncClient.cpp`,
`src/network/HttpDownloader.cpp`, `src/activities/browser/OpdsBookBrowserActivity.cpp`,
`src/activities/reader/KOReaderSyncActivity.cpp`, `src/activities/settings/KOReaderAuthActivity.cpp`.

## 46. X3 OPDS-feed-stall analysis (from collected `opds_debug.log`, 602 lines / ~19.5min trace) — CONCLUSIVE, feasibility LOW

**Root mechanism confirmed:** a live TLS session permanently consumes ~55-60KB of the single
unified heap (no PSRAM on C3 — empirically `MALLOC_CAP_INTERNAL == MALLOC_CAP_8BIT`, refining
the Appendix's "internal vs 8-bit" framing: there is no separate spare pool, everything craters
together). During transfer, free heap sits at 5-13KB with 2-6KB largest contiguous block
(median 2548B across 283 stall samples) — too small for a WiFi RX buffer (~1.6KB) to allocate
contiguously, so frames drop and TCP RTO stalls each read 13-114s (avg 23.6s). One trace: 283
stalls totaling ~1h51m cumulative dead time; ~38KB transfers took 56s-5.8min (109-700 B/s vs
X4's ~63KB/s).

**New finding beyond the Appendix:** the failure isn't read-loop-specific — it spans
connect-establishment and write/PUT paths too (9 outright `ESP_ERR_HTTP_CONNECT` + 12 mid-transfer
read errors observed; see §47 for the specific KOSync PUT failure this surfaced). Rules out any
read-loop-only firmware mitigation.

**Verdict:** feasibility to fix in firmware **LOW** (at or below the Appendix's prior 20-30%
estimate — arguably lower given there's no spare memory pool to shift load into, and the issue
isn't confined to one transport direction). Recommendation: don't invest further firmware effort;
practical workarounds remain an HTTP mirror or smaller server pages (Appendix). A speculative
pre-allocation idea was assessed at <10% likely to help and not pursued.

**Files:** none (analysis only, against `_X3_https/opds_debug.log`).

## 47. KOSync bookmark PUT failing 3/3 with `ESP_ERR_HTTP_CONNECT` right after a successful GET — FIX REVERTED (proven ineffective)

**Symptom:** In the collected trace, `BOOKMARKS_GET` succeeded (200 OK, slowly) but the
immediately-following `BOOKMARKS_PUT` failed all 3 retry attempts with `ESP_ERR_HTTP_CONNECT`
(0x7002 = 28674 — verified against the actual ESP-IDF `esp_http_client.h`, not assumed).

**Original theory (now falsified — see Addendum):** `createClient` doesn't set
`config.keep_alive_enable` — every call tears down and opens a fresh TCP+TLS connection. The
PUT's `createClient` ran ~19ms after the GET's `esp_http_client_cleanup`, colliding with the
still-releasing socket/TLS-session/DNS-resolver state from the prior connection. Heap looked
healthy (54KB free / 45KB largest) at the moment of the PUT's connect failure, which seemed to
rule out the Appendix's heap-starvation mechanism for this specific failure — both GETs in the
failing block succeeded (slowly, 200 OK), only the PUT failed 3/3, which argued against a
cold-WiFi-link explanation too.

**Original fix (now reverted):** `vTaskDelay(pdMS_TO_TICKS(800))` inserted between the
bookmark-merge (`bmSynced = true`) and the `updateBookmarks` PUT call — intended to give the
GET's connection time to fully release before the PUT opened a new one.

**Addendum (2026-06-08) — REVERTED after a fresh trace falsified the theory:** A new
`opds_debug.log` capture (10 boot sessions) included two sync attempts running the *fixed*
firmware (identified by the ~819-821ms gap matching `vTaskDelay(800)`). **Both still failed PUT
3/3 with the identical `ESP_ERR_HTTP_CONNECT`** — the delay changed nothing. Building a
session-by-session table across all 10 sessions revealed the real correlate: not GET→PUT timing,
but whether **every** connect in a sync session lands fast (<25ms — only session 1, the lone
success) or **all** of them land slow (1.6-4.4s, sessions 2-10, all failing at the connect stage
with `HTTP_EVENT_ON_CONNECTED` never firing). That's the *same* X3 internal-SRAM/heap-fragmentation
hardware condition from §46/the Appendix — a uniform per-session hardware state, not a
fixable stale-connection timing race. Reverted in commit `11656ad9` (the
`vTaskDelay`/comment block removed; `syncBookmarks` now goes straight from merge to PUT as
before). No firmware-level fix is known for this — see §46's LOW-feasibility verdict, which
now also covers this PUT-specific symptom.

**Files:** `src/activities/reader/KOReaderSyncActivity.cpp` (delay removed, commit `11656ad9`).

## 48. OPDS download screen — show full book title instead of single-line ellipsis truncation — FIXED

**Symptom:** The "Downloading…" screen showed the book title via
`renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40)` on one line —
long titles were cut short with an ellipsis, hiding the actual book name from the user.

**Fix (`OpdsBookBrowserActivity::render`, `DOWNLOADING` branch):** Replaced the single-line
`truncatedText` + fixed-offset draw with `renderer.wrappedText(UI_10_FONT_ID, statusMessage.c_str(),
pageWidth - 40, 2)` — wraps the title over up to 2 centered lines (the same helper §38's title-
overflow fix and `CrashActivity` use; it falls back to ellipsis-truncating only the rare title
that can't fit even 2 lines). The progress bar / byte-count line position (`barY`) is now
computed from the actual number of wrapped lines × `getLineHeight()`, so it never overlaps the
title block regardless of whether the title needed 1 or 2 lines. Orientation-aware throughout
(`pageWidth`/`pageHeight`/`getLineHeight`, no hardcoded dimensions).

**Files:** `src/activities/browser/OpdsBookBrowserActivity.cpp:305-329`.

## 49. `esp_http_client` `buffer_size` (RX) bump to ~16KB for OPDS GET — analyzed, NOT recommended

User asked whether raising `HttpDownloader.cpp:23`'s `HTTP_RX_BUF` (4096) to ~16354 would speed
up OPDS GETs. Traced the actual mechanism in `esp_http_client.c` (the framework source, not
assumed): `config.buffer_size` becomes `client->buffer_size_rx`, a single `malloc` at client
init (`esp_http_client.c:920-921`); each `esp_http_client_read` clamps its transport read to
`min(need_read, buffer_size_rx)` (`esp_http_client.c:1335-1336`) — and `need_read` is bounded by
**our own** `READ_CHUNK = 2048` (`HttpDownloader.cpp:30,193`), which is already smaller than the
current 4096 buffer. So the clamp never engages today, and a bigger `buffer_size_rx` changes
nothing about body-read throughput or "splitting" — `READ_CHUNK` is the actual limiter (kept at
2KB deliberately, per `HttpDownloader.cpp:95-98`, so it can be carved from contiguous heap before
the TLS handshake fragments it).

**What it WOULD do:** balloon the one-time `malloc(buffer_size_rx)` from 4KB → ~16KB — an extra
~12KB grabbed at exactly the moment the ~55-60KB TLS session is also competing for the same
scarce contiguous heap that §46 showed craters to 2-6KB largest block during stalls. That raises
`ESP_ERR_HTTP_CONNECT`/OOM risk (the `MIN_CONTIGUOUS_HEAP_FOR_TLS` preflight, `OpdsBookBrowserActivity.cpp:618,625`,
would trip more often), making things worse, not better. **Verdict: don't change it.**

**Files:** none (analysis only).

## 50. EPUB reader — "select bookmark" prompts for a return mark before jumping (mirrors §15)

**Goal:** picking a bookmark from "View Bookmarks" can carry the reader far from the current
spot with no easy way back. §15 already added this guard to "Select Chapter" and "Go to %"; this
extends the same guard to bookmark selection.

**Change (`EpubReaderActivity::onReaderMenuConfirm`, `VIEW_BOOKMARKS` case):** navigation is now
wrapped in a `doNavigate` lambda (the original body: `removeReturnMarkAt` + spine/progress jump +
`section.reset()`). Before calling it, if `section->pageCount > 0` and
`!BOOKMARKS.hasBookmarkForPage(currentSpineIndex, currentProgress, pageCount)` — i.e. the current
page carries neither a hollow return mark nor a normal bookmark — shows the existing
`ConfirmationActivity`/`STR_CONFIRM_ADD_RETURN_MARK` prompt; confirming calls
`addBookmark(/*returnMark=*/true, lightRefresh=true)` before `doNavigate()`, declining just
navigates. No "actually moving" check needed (unlike §15's chapter/percent cases): if
`hasBookmarkForPage` is false for the current page, the selected bookmark — which IS a bookmark
somewhere — cannot be the one covering the current page, so the jump is guaranteed to go
elsewhere. Reuses §15's string/activity wiring entirely — no new strings, no cache/format version
bump.

**Files:** `src/activities/reader/EpubReaderActivity.cpp:967-1003`.

---

## 51. Reader — "No Dictionary installed" popup on hold-Confirm (upstream)

**Goal:** when hold-Confirm fires the dictionary gesture but no prepared dictionary is installed, surface a brief popup instead of silently doing nothing.

**`EpubReaderActivity.cpp` / `.h`:** the `HOLD_CONFIRM_DICTIONARY` branch in `loop()` now has two sub-paths:
- `Dictionary::exists(cachePath)` → open word-select (unchanged).
- else → set `showNoDictionaryMessage = true`, `ignoreNextConfirmRelease = true`, record `noDictionaryMessageTime = millis()`.

Timer dismissal uses `DICTIONARY_MESSAGE_DURATION_MS = 1500` ms — a new constant in `ReaderUtils.h` (bookmark messages stay at `BOOKMARK_MESSAGE_DURATION_MS = 2500`). `render()` draws `STR_DICT_NO_DICT_SET` popup when the flag is set.

**`EpubReaderActivity.h`:** `bool showNoDictionaryMessage = false` + `unsigned long noDictionaryMessageTime = 0UL` added (auto-merged cleanly with the existing local bookmark-message flags).

**i18n:** `STR_DICT_NO_DICT_SET` already in `english.yaml` from prior dictionary work; other languages fall back until translated.

**Source:** upstream PR #12 (`feat-dictionary`, WuTofu, commits `1b496e68` + `7cc10123`).

---

# Part I — X4 clock-availability, bookmark-crash hardening, and the image-decode/heap-fragmentation chain

## 52. X4 home top-bar clock — survives the silent restart + auto-syncs on every boot — FIXED (device-confirmed)

**Symptom:** on X4 (no DS3231 RTC) the home top-bar date/time showed on the WiFi screen but vanished on
return home, and never appeared after sleep.

**Root causes & fixes:**
- **Lost across the silent restart (Fix B).** The WebServer→home flow does `silentRestart()` → `ESP.restart()`
  (`RTC_SW_CPU_RST`), wiping the RAM-only `_ntpConfigured`. Added `HalClock::persistTimeAcrossReboot()` —
  stashes the synced epoch in `RTC_NOINIT_ATTR` (survives `ESP.restart()`, not power loss), called before each
  `silentRestart*` in `main.cpp`. `HalClock::begin()` (X4 branch) adopts a still-valid RTC-backed clock, else
  restores the stashed epoch via `settimeofday`, then **consumes the stash** (clears magic) so a later
  deep-sleep wake (unknown elapsed time) re-syncs instead of restoring a stale time.
- **No sync on wake (Option 2).** Extracted the QuickResume-only background NTP task into
  `maybeStartBackgroundNtpSync()` (`main.cpp`), now called on **both** QuickResume **and** Splash boots, gated on
  `!hasHardwareRtc() && !isSystemTimeValid()` + clock-on (`homeTopBarClock||homeTopBarDate`) + a saved WiFi SSID.
  The user's device uses full (DARK) sleep → wake = Splash, so the Splash hook is what makes it work.
- **Problem A (slow SNTP).** `syncFromNTP(uint32_t maxWaitMs = 5000)` — the background task passes `20000` so a
  slow SNTP packet isn't cut off when the task tears WiFi down; UI callers keep 5s.

**Files:** `lib/hal/HalClock.{h,cpp}`, `src/main.cpp`. See memories `project-ntp-x4-clock`, `project-home-top-bar-clock`.

## 53. X4 reader status-bar clock/date settings unlocked — FIXED

**Goal:** `Settings > Reader > Customise Status Bar` hid the clock/date items on X4 (gated on `halClock.isAvailable()`,
false until NTP syncs). Since X4 can now get time over WiFi, the menu must show regardless of current sync state.

**Change (`StatusBarSettingsActivity.cpp`):** gate is now `halClock.isAvailable() || !halClock.hasHardwareRtc()` —
X4 (no hardware RTC) always shows the full menu so the user can enable the clock and trigger a sync. `ClockSyncActivity`
is WiFi+NTP based, so all items work on X4. X3 unchanged.

## 54. Bookmark corrupt-file crash (`abort`) on one book — FIXED (device-confirmed)

**Root cause (decoded from `crash_report.txt`):** NOT OOM (112KB free). `BookmarkStore::readFromFile` →
`serialization::readString` did `s.resize(len)` with a garbage `len` from a corrupt bookmark `.bin` →
`std::length_error` → `terminate` → `abort` (under `-fno-exceptions`).

**Change:** both `serialization::readString` overloads now bound `len` by the bytes remaining in the
stream/file (a serialized string can't exceed what's left) and return `bool`; on a bad length they leave the
string empty and return false. Hardens **all** file deserialization, not just bookmarks. `readFromFile` detects
the corruption (false return), closes + `Storage.remove`s the bad file so a clean store regenerates, and the
book opens (bookmark load failure is non-fatal). Host stubs gained `available()`.

**Files:** `lib/Serialization/Serialization.h`, `src/BookmarkStore.cpp`, `test/*/stubs/HalStorage.h`.

## 55. Image streaming px13n cache — oversized images cache at full 4-level grayscale — FIXED (device-confirmed)

**Symptom:** on X4 with AntiAlias on, a full-page cover took *minutes*: the grayscale render walks ~10 strips × 2
planes, and an uncached image **re-decodes the JPEG in every strip** (~20×). Caching was skipped because the 2bpp
px13n buffer (~80KB) exceeds both the cap and X4's largest free block (~63KB), so the full-RAM `PixelCache` can't
allocate.

**Change:** added `StreamingPixelCache` (`PixelCache.h`) — a small (128-row) band buffer that flushes finalized
rows to SD **during the single decode**, producing the exact same `.px13n` file (reader unchanged). The JPEG
callback already feeds the cache writer in raster row order; `DirectCacheWriter` was unified to drive either the
full-RAM buffer (small images, ≤48KB) or the streaming writer (larger), flushing rows strictly below the current
block's top (finalized by completed MCU-rows) so same-MCU-row blocks never hit a flushed row. Progressive JPEGs
(non-raster) fall back to no-cache; partial/incomplete streams are removed. Result: one decode, then every
strip/pass/reopen reads the cache. **Device log:** `Streaming cache to SD: 464x701 (81316 bytes)` → one 2.8s
decode → ~13× `Cache render complete`.

**Files:** `lib/Epub/Epub/converters/PixelCache.h`, `DirectPixelWriter.h`, `JpegToFramebufferConverter.cpp`.

## 56. JPEG "image too large" rejected valid covers + failed-decode retry storm — FIXED (device-confirmed)

**Two bugs feeding an endless failed-decode loop:**
- The max-pixel check (`MAX_SOURCE_PIXELS = 3145728`) ran on **raw source** dims (e.g. 1478×2367 = 3.5M px) *before*
  JPEGDEC's built-in 1/2–1/8 downscale. Moved it to the **scaled decode grid** (raw dims keep only a 30000/side
  overflow guard), so a cover that downscales to ~219K px now decodes.
- A failed decode wasn't remembered, so an un-decodable image re-decoded on every render pass (~20×, tens of
  seconds, heap churn). Added `ImageBlock::decodeFailed` — retry at most once per section view. Also: skip image
  decode entirely during the font-prewarm **scan pass** (`isFontCacheScanning()`), which draws nothing.

**Files:** `lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp`, `lib/Epub/Epub/blocks/ImageBlock.{h,cpp}`.

## 57. Home cover-thumb generation retried every visit on failure — FIXED

**Change (`Epub::generateThumbBmp`):** a failed thumb (heavy inflate + JPEG/PNG decode) is recorded in a
**session-only** `s_failedThumbGen` set so the home screen stops re-running it on every visit. In-RAM (not an SD
marker) so a fresh-heap reboot retries once — no permanent lockout of a cover that only failed transiently under
fragmentation.

**Files:** `lib/Epub/Epub.cpp`.

## 58. "Out of bounds" on reader = heap fragmentation, not SD / section logic — RESOLVED via §55–57

**Investigation:** the reader's `STR_OUT_OF_BOUNDS` screen (4 branches in `EpubReaderActivity::render`) fired with
free heap ~25KB. Per-transition `[HEAP]` logging proved **no leak** (onExit recovers to ~120KB). The real cause:
the ZIP **inflate window needs 32768 bytes contiguous** (`InflateReader::init`), but `MaxAlloc` (largest free
block) dipped below 32KB — dynamic **fragmentation** driven by the repeated failed-image / thumb decode churn
(boot `MaxAlloc` is ~114KB; no fixed fragmenter). Killing the churn (§55 streaming, §56 scaled-limit +
`decodeFailed`, §57 thumb marker) raised the typical floor, but a later device test still hit "out of bounds":
opening an *uncached* book whose **section build** thrashes the heap dropped `MaxAlloc` to ~23–29KB and **stuck**
there (110KB free, idle) — the section build's own 32KB inflate malloc/free is the dominant fragmenter. So §55–57
reduced but did not eliminate it.

## 59. Reserve the DEFLATE inflate window in BSS — durable "out of bounds" fix (Option A) — DEVICE-CONFIRMED

**Change (`InflateReader`):** the 32KB DEFLATE back-reference window is now a file-scope BSS array
(`s_inflateWindow[32768]`) handed out via an `std::atomic_flag` in-use guard, instead of `malloc`/`free` on every
`init(true)`. Callers get a guaranteed-contiguous window regardless of heap fragmentation, and the per-section-build
32KB malloc/free churn (itself the main fragmenter, confirmed by `[FRAG]` probes: `MaxAlloc` 47092→23540 *during*
the section build) is gone. A concurrent inflate (e.g. web-server task while reading) falls back to `malloc` —
never worse than before. `~InflateReader()`→`deinit()` releases the flag (RAII); `decomp` stays at offset 0 for the
uzlib callback cast.

**Device result:** 0 "out of bounds" across 7 uncached section builds, 19 pages rendered, no crash.

**Cost (RAM):** +32KB static (heap pool 223→191KB; build RAM 31.4%→41.4%). The device is at the memory edge, so the
tighter pool surfaces **graceful** secondary OOMs during heavy section builds: font `buildAdvanceTable`'s 16KB
codepoint buffer falls back to the compact mini-kern (text renders fine), and the JPEG decoder (needs 36–53KB
contiguous) skips an occasional inline image / cover thumb (handled by §56/§57). No crashes; reading works
throughout. Net: a *fatal* failure (couldn't open uncached books) traded for *cosmetic* degradation.

**Follow-up:** see §60 — `buildAdvanceTable` now borrows the reserved window, recovering font quality.

**Files:** `lib/InflateReader/InflateReader.{cpp,h}`.

## 60. Lend the reserved inflate window as scratch to recover font quality — DEVICE-CONFIRMED

**Goal:** §59's +32KB reservation tightened the heap so `SdCardFont::buildAdvanceTableRange`'s ~16KB codepoint
buffer (`uint32_t[4098]`) failed on heavy builds → fell back to mini-kern. Recover the full advance table without
adding RAM.

**Change:** `InflateReader` gained `acquireScratch(need)` / `releaseScratch()` — lend the reserved 32KB window
(now `alignas(8)` for the wider-type cast) via the same atomic in-use flag. `buildAdvanceTableRange`, when its
`new` fails, borrows the window instead of returning -1. Safe because the window is free here — section-build
inflate completes before layout/prewarm, plain render doesn't inflate, and font glyph decode uses
`FontDecompressor` (not `InflateReader`). A concurrent inflate (web task) → `acquireScratch` returns null → today's
graceful mini-kern fallback. Single linear exit → always released.

**Device result:** 0 `buildAdvanceTable` failures (was many), 0 "out of bounds", 24 pages rendered. RAM unchanged
(reuses the §59 window).

**Residual (graceful, not regressions):** under the tighter pool, the home recent-book cover buffer (32640 B) and
the §55 streaming-cache band (~15KB) still OOM occasionally — the cover card skips its image, and an oversized
image falls back to live re-decode (slower, uncached). Both non-fatal; candidates for the same borrow-the-window
trick later.

**Files:** `lib/InflateReader/InflateReader.{cpp,h}`, `lib/EpdFont/SdCardFont.cpp`.

## 61. Home covers blank on Vega = cover-thumb JPEG guard rejected valid covers under fragmentation — RESOLVED

**Symptom:** some books with a real cover image showed only a placeholder on the home screen (Vega theme).

**Diagnosis:** added a temporary SD-routed probe (`SdDebugLog` in `HomeActivity::loadRecentCovers` +
`Epub::generateThumbBmp`, X3-readable via `/opds_debug.log` — X3 has no serial). Device log showed the cover
`cover.jpeg` was valid; failure was the JPEG decoder heap guard:
`Not enough heap for JPEG decoder (48384 free, need 53248)`. The guard checked **total free** against
`JPEG_DECODER_SIZE (20KB) + 32KB` slack. But the real constraint is a single ~20KB contiguous block (the JPEGDEC
object); `MaxAlloc` was 40948 — plenty. §59's −32KB reservation made the false rejection routine. All thumb-path
buffers are `makeUniqueNoThrow` + null-checked → a shortfall already fails gracefully (placeholder), never crashes.

**Change (`JpegToBmpConverter.cpp`):** replaced the total-free guard with a **largest-contiguous-block** guard —
`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < JPEG_DECODER_SIZE + 8*1024` (28672). Targets the true
fragmentation constraint; the failing cover (MaxAlloc 40948 > 28672) now decodes. Added `#include <esp_heap_caps.h>`.
The SD cover probe was stripped after diagnosis (kept the `LOG_DBG/LOG_ERR` lines and §57 session-skip marker).

**Batch committed alongside §61** (device-pending fixes from §55–60 follow-up work):
- **#1 thumb scale-aware** (`JpegToBmpConverter.cpp`): pick JPEGDEC 1/2..1/8 denom so the decode grid fits
  `MAX_IMAGE_WIDTH 2048` / `MAX_IMAGE_HEIGHT 3072` instead of rejecting large covers outright.
- **#3 keep cover path** (`HomeActivity.cpp`): stop wiping the persisted `coverBmpPath` on a (often transient,
  fragmentation-driven) thumb-gen failure — a later attempt on a freer heap regenerates it; meanwhile the tile
  shows a placeholder rather than permanently losing the cover.
- **streaming-band borrow** (`PixelCache.h`): `StreamingPixelCache` band malloc falls back to
  `InflateReader::acquireScratch()` (the §59/§60 reserved window) when malloc fails, so large grayscale images
  cache instead of re-decoding every page.
- **heap-floor guard** (`Section.cpp`): before the chapter parser, bail (clean `return false`, remove temp files)
  if `esp_get_free_heap_size() < 48KB` — turns the §59-era parser `bad_alloc` abort into a graceful skip.

**Files:** `lib/JpegToBmpConverter/JpegToBmpConverter.cpp`, `src/activities/home/HomeActivity.cpp`,
`lib/Epub/Epub/converters/PixelCache.h`, `lib/Epub/Epub/Section.cpp`.

## 62. Reader "out of bounds" on CSS-heavy books (Project Hail Mary) = CSS heap pressure + over-conservative parse floor — RESOLVED

**Symptom:** opening *Andy Weir - Project Hail Mary* dropped straight to the "out of bounds" screen
(`[E1 spine=N heap=M]`). Front-matter and chapters failed to index. Unaffected books opened fine.

**Diagnosis (device logs, three iterations):** the book carries **294 CSS rules** (vs ~63 for a typical book).
The section parser logged `Low heap (38352 < 49152) before parse — skip to avoid OOM crash` — the §61-batch
heap-floor guard (`Section.cpp`, blanket 48KB) bailing because resident CSS ate the headroom, so no pages built →
page index out of range → "out of bounds". Two compounding costs:
1. **CSS footprint** — ~234 useful rules × ~100B `CssStyle` + container overhead held resident the whole parse.
2. **Container fragmentation** — rules lived in `std::unordered_map`, i.e. one separately-malloc'd node per rule;
   234 tiny allocations fragmented the heap and added ~5–6KB node/bucket overhead.

**Changes (three landed together):**
- **Drop empty CSS rules** (`CssParser.cpp`, `CSS_CACHE_VERSION` 6→7): skip storing any rule whose `CssStyle`
  sets no e-ink-relevant property. `resolveStyle` only ever `applyOver()`s a matched rule and `applyOver(empty)`
  is a no-op, so an absent selector and a present-but-empty one are identical in output — safe to drop. Filtered
  at both parse-time store and cache load. Result: 294→234 rules, free heap 38352→45700.
- **`unordered_map` → sorted flat `std::vector<pair<string,CssStyle>>`** (`CssParser.{h,cpp}`): one contiguous
  allocation instead of 234 fragmenting nodes; lookups via `findRule()` binary search (`std::lower_bound`),
  inserts keep the vector ordered, cache load `reserve()`s then `std::sort`s once. Saves the per-node overhead and
  defragments. Sections 3/4/5 then parsed and rendered.
- **Size-aware parse floor** (`Section.cpp`): replace the blanket 48KB with
  `required = clamp(36KB + inflatedHtmlBytes, 36KB, 48KB)`. The parser's peak working set scales with section
  size, so a 3KB front-matter page no longer needs the same headroom as a 50KB chapter. Last blocker was index 2
  (3165B HTML) failing by **712 bytes** (48440 < 49152); it now needs only ~40KB. Large chapters keep the 48KB
  cap (crash margin unchanged). Log now prints `html=N` for the failing section.

**Notes:** `buildFailedSpine` (`EpubReaderActivity.cpp`) is RAM-only and cleared by navigating away — no SD lockout,
no `.crosspoint/` delete needed. Empty-rule drop is layout-neutral (no-ops removed) so `SECTION_FILE_VERSION` stays
25; only `CSS_CACHE_VERSION` bumped (self-invalidates `/css_rules.cache`). Front-matter is fixed device-confirmed
through §62 iterations; large novel chapters now have the best odds yet but sit near the 48KB cap (~50KB free) —
next lever if one trips: reclaim font-cache heap during parse, or lower the cap.

**Also in this batch — Home cover-thumb scale-aware decode (§61 #1 follow-through), device-confirmed:** the thumb
JPEG path (`JpegToBmpConverter::jpegFileToBmpStreamInternal`) only downscaled the decode grid to fit the
`MAX_IMAGE` safety cap, so a 1456-wide cover decoded at full width → ~23KB contiguous MCU row buffer that failed
under fragmentation (or tripped the §61 28KB block guard), stranding the home tile on a placeholder. Now
**target-aware**: compute the desired output first, then pick the largest JPEGDEC 1/8..1/2 denom whose grid still
covers the output. A 226px thumbnail decodes at ~183px grid (~3KB row buffer, was ~23KB) and ~64× fewer pixels.
Device log confirms: `JPEG decode uses 1/8 source: 183x275 … success: yes`.

**Files:** `lib/Epub/Epub/css/CssParser.{h,cpp}`, `lib/Epub/Epub/Section.cpp`,
`lib/JpegToBmpConverter/JpegToBmpConverter.cpp`.
