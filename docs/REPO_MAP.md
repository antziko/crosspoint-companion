# Repo Map

Orientation doc for Claude Code sessions. Companion to `CLAUDE.md` (rules) and `SCOPE.md` (what's in/out of scope). For cache file formats see `docs/file-formats.md`; for activity navigation see `docs/activity-manager.md`.

## 1. What this project does

Open-source e-reader **firmware** for the ESP32-C3-based Xteink X4 / X3 (800×480 mono E-Ink, ~380KB RAM, no PSRAM, 16MB flash, SD card storage). Renders EPUB 2/3, `.xtc/.xtch`, `.txt`, `.bmp`. Features: hyphenation, kerning, footnotes, bookmarks, go-to-percent, focus reading, orientation control, offline StarDict dictionary lookup, reading-time stats (per-book + global, heatmap on X3), KOReader progress sync, custom SD fonts, tilt page turn (X3), and wireless workflows (file-transfer web UI, EPUB optimizer, web settings API, OTA, Calibre/OPDS).

Hard constraint: **stability under ~380KB RAM**. Single 48KB framebuffer. See `CLAUDE.md` Resource Protocol before allocating.

## 2. Main folders

| Path | Purpose |
|------|---------|
| `src/` | Firmware app code (entry point, activities, networking, utils). |
| `src/activities/` | UI screens via Activity lifecycle (`onEnter`/`loop`/`onExit`). Subdirs: `home`, `reader`, `settings`, `network`, `browser`, `boot_sleep`, `util`. `ActivityManager` is the navigation stack. |
| `src/activities/reader/` | EPUB/TXT/XTC readers + dictionary lookup + reader menus/options. Biggest activity surface. |
| `src/network/` | Web server, HTTP downloader, OTA updater, firmware flasher, WebDAV. HTML UIs generated into `src/network/html/*.generated.h`. |
| `src/util/` | Shared helpers: dictionary core, bookmarks, lookup history, screenshots, string/URL utils, `Task.h`. |
| `src/platform/` | Low-level platform shims (e.g. efuse check skip). |
| `src/components/` | Icons + themes (`themes/{lyra,roundedraff,vega}` + `BaseTheme`; `CrossPointSettings::UI_THEME` enum lists all 5: Classic/Lyra/Lyra3Covers/RoundedRaff/Vega). |
| `lib/` | Internal libraries. Key ones below. |
| `lib/hal/` | **Hardware Abstraction Layer** — `HalStorage`/`HalGPIO`/`HalDisplay` etc. ALL SDK access routes here (SdFat not thread-safe — see `CLAUDE.md`). |
| `lib/Epub/` | EPUB parse + layout engine; `.crosspoint/` cache writer (`book.bin`, `section.bin`). |
| `lib/GfxRenderer/` | E-Ink rendering primitives, grayscale buffer handling. |
| `lib/EpdFont/` | Bitmap font format + builtin fonts. |
| `lib/I18n/` | Translations. Edit `translations/*.yaml`, regen with `scripts/gen_i18n.py` (generated files gitignored). |
| `lib/DictHtmlRenderer/` | Renders dictionary entry HTML. |
| `lib/Xtc/`, `lib/Txt/`, `lib/OpdsParser/`, `lib/KOReaderSync/`, `lib/ZipFile/`, `lib/InflateReader/`, `lib/uzlib/`, `lib/expat/`, `lib/JsonParser/`, `lib/*ToBmpConverter/` | Format/codec/parser support libs. |
| `lib/Memory/` | `makeUniqueNoThrow` — the mandated allocation helper. |
| `open-x4-sdk/` | Vendored low-level SDK (EInkDisplay, InputManager, BatteryMonitor, SDCardManager). Wrap via HAL — do not call directly. |
| `scripts/` | Build/codegen/tooling (Python). i18n, HTML build, hyphenation trie, font manifest, dictionary tools, unit-test runner. |
| `test/` | Host-side gtest suites (no hardware). |
| `docs/` | Format specs + feature/dev docs. |
| `data/` (referenced) | HTML source templates for `scripts/build_html.py`. |

## 3. Main entry points

- **`src/main.cpp`** (~686 lines):
  - `setup()` (~L338): Serial → `HalSystem::begin()` → GPIO/power/tilt/clock → `Storage.begin()` → settings load → `setupDisplayAndFonts()` → first activity.
  - `setupDisplayAndFonts()` (~L304): loads ~80+ global font objects (gated by `OMIT_FONTS`), display init.
  - `loop()` (~L545): drives `ActivityManager`.
- **`src/activities/ActivityManager.{h,cpp}`**: navigation stack. `goToReader/goToSettings/goToFileBrowser/goHome/...`, `replaceActivity`, push/pop. Runs a render task (`renderTaskLoop`). This is the hub for screen flow.
- **`src/activities/home/HomeActivity`**: default landing screen.

## 4. Build / test / run

```bash
# Firmware build (PlatformIO; envs: default, gh_release, gh_release_rc, slim)
pio run
pio run -e gh_release
pio run -t upload            # flash device
pio device monitor           # or: python3 scripts/debugging_monitor.py

# Host unit tests (gtest, no hardware — agents CAN run these)
pio run -t unit-tests
# or direct:
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure -j

# Quality
pio check
find src -name "*.cpp" -o -name "*.h" | xargs clang-format -i

# Codegen (run after editing sources, not generated files)
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/   # i18n tables
# HTML headers auto-built by build_html.py during pio pre-build
```

## 5. Important config files

| File | Role |
|------|------|
| `platformio.ini` | Main build config + critical `-D` flags (single-buffer mode, USB CDC, XML limits). Committed. |
| `platformio.local.ini` | Personal overrides (ports, debug flags). Gitignored — never commit. |
| `partitions.csv` | ESP32 flash partition layout. |
| `CLAUDE.md` | Project rules (RAM protocol, HAL, allocation, dictionary rules). Symlink → `.skills/SKILL.md`; edit the real target. |
| `SCOPE.md` / `GOVERNANCE.md` | Feature scope + project governance. |
| `test/CMakeLists.txt` | Host test build; pins gtest version. |
| `scripts/register_unit_tests_target.py` | Registers `pio run -t unit-tests`. |

## 6. Data flow

**Book open → read:**
`HomeActivity`/file browser → `ActivityManager.goToReader(path)` → `EpubReaderActivity` → `lib/Epub` parses EPUB, checks `.crosspoint/epub_<hash>/` cache (`book.bin` v6, `section.bin` v25). Cache miss/invalid → re-layout, write cache. Pages → `GfxRenderer` → `HalDisplay` → E-Ink. Progress debounced to `progress.bin`. Cache key = `std::hash(filepath)`; move/rename = new hash = lost progress.

**Cache invalidation:** format version bump, render-setting change (font/size/spacing/margin), orientation/viewport change, or file content change. See `CLAUDE.md` Cache Management.

**Input:** physical buttons → `InputManager` (SDK) → `HalGPIO` → `MappedInputManager` (logical buttons, user-remappable + orientation transforms) → activity. Use `MappedInputManager::Button::*`, never raw GPIO.

**Non-reader orientation control:** `SETTINGS.displayOrientation` is scoped to four list/browse screens only (`FileBrowserActivity`, `RecentBooksActivity`, `OpdsBookBrowserActivity`, `ReadingStatsActivity`) — Home/Settings/File Transfer always stay Portrait. Each screen self-manages: applies the setting in `onEnter`, force-resets to Portrait in `onExit` (mirrors the reader's per-activity orientation handling, just non-ambient). Side Up/Down route through `ReaderUtils::resolveSideNavAction` — short press = list-step, long press while `SETTINGS.sideLongPressButtonBehavior == ORIENTATION_CHANGE` = `ReaderUtils::cycleDisplayOrientation` (cycles + persists + applies immediately, mirroring the reader's hold-to-rotate).

**All SD I/O** serialized through `HalStorage` mutex (`Storage` singleton). `HalFile`, not raw `FsFile`.

**Dictionary lookup:** reader word-select → `DictionaryLookupController` → `DictLookupTask` (off-UI FreeRTOS task) → `Dictionary` (StarDict over prepared offset files) → `DictHtmlRenderer` → definition activity. Prep is one-time via `DictPrepareTask` / `scripts/dictionary_tools.py prep`. If no prepared dictionary is installed, hold-Confirm shows a brief `STR_DICT_NO_DICT_SET` popup (`showNoDictionaryMessage`, `DICTIONARY_MESSAGE_DURATION_MS = 1500 ms` from `ReaderUtils.h`) instead of opening word-select.

**Networking:** `NetworkModeSelectionActivity` → WiFi → `CrossPointWebServer` (file transfer, settings API, WebDAV) / `HttpDownloader` (OPDS/Calibre) / `OtaUpdater`.

**Reading stats:** `EpubReaderActivity` tracks session time, dated via `HalClock::getLocalDateTime(SETTINGS.clockUtcOffsetQ, ...)` (local-calendar day/time — UTC-offset + midnight-rollover corrected, *not* raw RTC reads) → `BookReadingStats` (`<cachePath>/stats.bin`, v3) + `GlobalReadingStats` (`.crosspoint/global_stats.bin`, v1) → `ReadingTimeHistory` (dated weekly/monthly/yearly + 730-day heatmap, X3-only; `<cachePath>/book_time_history.bin` / `.crosspoint/global_time_history.bin`, `HISTORY_FILE_VERSION` v2 — heatmap is 2-bit/day `HeatmapLevel{None,Light,Moderate,Heavy}` classified from each day's *accumulated* reading seconds, not a presence bitmap) → rendered by `BookStatsActivity` / `ReadingStatsActivity` (`ActivityManager::goToReadingStats`) as a 4-shade grid (white/light-gray/dark-gray/black). Covered by `test/reading-time-history/` gtest suite.

## 7. Common change points

- **New UI screen** → add Activity under `src/activities/<area>/`, wire into `ActivityManager`. Follow lifecycle + free everything in `onExit()`.
- **Reader behavior** → `src/activities/reader/` (esp. `EpubReaderActivity`, `ReaderOptionsActivity`, `ReaderSettingsIO`).
- **Rendering/layout** → `lib/Epub/` (bump cache version on binary-format change!) or `lib/GfxRenderer/`.
- **New setting** → `CrossPointSettings` (singleton `SETTINGS`); throttle SPIFFS writes; may invalidate cache.
- **UI text** → add `STR_*` to `lib/I18n/translations/english.yaml`, regen, use `tr(STR_*)`. Never hardcode.
- **Web UI** → edit `data/html/`, not `*.generated.h`.
- **Button mapping** → `src/MappedInputManager.cpp`.
- **Dictionary** → `src/util/Dict*` (incl. `DictionaryRegistry` — scans SD for installed StarDict dicts, mirrors `SdCardFontRegistry`) + `lib/DictHtmlRenderer/` (see `CLAUDE.md` non-negotiable dictionary rules: plan-mode first, verify before assuming).
- **Reading stats** → `src/activities/reader/{BookReadingStats,GlobalReadingStats,ReadingTimeHistory,BookStatsActivity,ReadingStatsActivity}`. Bump `STATS_FILE_VERSION`/`GLOBAL_STATS_FILE_VERSION` on binary-format change.
- **Home themes** → `src/components/themes/<name>/` (extend `BaseTheme`); register in `CrossPointSettings::UI_THEME` + `SettingsList.h`.
- **Orientation-toggle screens** → only the four browse/list screens named above follow `SETTINGS.displayOrientation`; adding a fifth means the same `onEnter`-apply / `onExit`-reset-to-Portrait pair + `ReaderUtils::resolveSideNavAction`/`cycleDisplayOrientation` wiring — never make orientation ambient/global (it would leak into Home/Settings/File Transfer).

## 8. Unknowns / risky areas

- **RAM ceiling (~380KB, single framebuffer)**: largest ongoing risk. Justify every heap alloc; use `makeUniqueNoThrow`; bare `new` aborts (no exceptions). Grayscale needs temp buffer + restore.
- **Heap fragmentation, not total-free, is the real limit**: under churn `MaxAlloc` (largest contiguous block) drops well below total free, so big single allocs (JPEG decoder ~20KB, inflate window 32KB, font advance table ~16KB) fail even with plenty free. Pre-alloc guards must check `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`, not `ESP.getFreeHeap()` (see §61 cover-thumb fix). The §59 reserved inflate window doubles as a shared big-transient scratch pool via `InflateReader::acquireScratch()` (font table §60, streaming-cache band §61). Prefer contiguous containers over per-node ones for many-element collections held during parse — §62 switched the CSS rule store from `unordered_map` (one malloc'd node per rule) to a sorted flat `std::vector` to cut fragmentation. The chapter-parse heap floor (`Section.cpp`) is size-aware: `clamp(36KB + inflatedHtmlBytes, 36KB, 48KB)` (§62) — small front-matter pages don't need a full chapter's headroom. See `SUMMARY.md` §55–62. The §59 window is now **heap-backed, not BSS** (§63): reserved once at boot via `InflateReader::ensureWindow()` while the heap is pristine (so it's still guaranteed contiguous), but freeable via `releaseWindow()` to hand 32KB back for a TLS handshake — KOSync releases it before its handshakes (its `onExit` silent-restart re-reserves it), which restored HTTPS progress sync after §59's +32KB cut the pool 223→191KB and started tripping the TLS heap preflight. `init(true)`/`acquireScratch` fall back to `malloc` when the window is released.
- **HTTPS handshakes are heap-starved on this device**: a TLS session peaks ~55-63KB and craters free heap to single-KB even with the window reclaimed, so HTTP requests are scheme-gated to skip the TLS-sized preflight (`heapOkForUrl` in `KOReaderSyncClient`, `minContiguousForUrl` in `OpdsBookBrowserActivity` — `http://` uses a small bar, `https://` the full TLS bar). Still unsolved (need framebuffer-to-heap or a from-source `MBEDTLS_DYNAMIC_BUFFER` build): KOSync `BOOKMARKS_PUT` failing on the 3rd back-to-back handshake (SUMMARY §47), and slow/marginal HTTPS OPDS (the window reclaim there was reverted — freeing a mid-session block ≠ pristine pool, so the handshake still craters).
- **SdFat thread-safety**: concurrent SD access panics FreeRTOS (issue #518). NEVER bypass `HalStorage` mutex.
- **ISR / flash-cache rules**: ISRs in `IRAM_ATTR`, their data in `DRAM_ATTR`; no mutex from ISR. See `CLAUDE.md` pitfalls.
- **RISC-V alignment**: no casting `uint8_t*` to wider type — `memcpy`. Affects all cache deserialization.
- **Cache versioning**: forget to bump `BOOK_CACHE_VERSION` / `SECTION_FILE_VERSION` on format change → silent corrupt renders. `docs/file-formats.md` currently LAGS code (says v5/v24; code is v6/v25) — trust the constants.
- **`string_view` null-termination**: not null-terminated; never pass `.data()` to C APIs / SdFat paths.
- **Orientation**: 4 modes (Portrait/Inverted/Landscape CW/CCW). Never hardcode 800/480; use renderer getters. Verify switch/case covers all 4.
- **Activity lifecycle leaks**: activities are heap-allocated and `delete`d on exit. Tasks must `vTaskDelete` and member `FsFile` must close in `onExit()`.
- **Generated files**: `*.generated.h`, `I18n*.{h,cpp}` are gitignored/regenerated — never hand-edit or commit.
- **Symlinked `CLAUDE.md`** → `.skills/SKILL.md`; edits must target the real file.
- **Two boards (X4/X3)**: X3 adds tilt sensor; confirm board-specific paths when touching input/power.
