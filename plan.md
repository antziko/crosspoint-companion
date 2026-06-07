# Reading Stats Feature — Plan & Status

## Phase 1 — Reading Stats Core ✅ DONE
- `BookReadingStats` v2 (`src/activities/reader/BookReadingStats.h/.cpp`)
  - Fields: `totalReadingSeconds`, `unattributedSeconds`, `avgSecondsPerForwardPage`, `paceSampleCount`
  - v1 fields `sessionCount`/`totalPagesTurned` dropped (not migrated — v1 files rejected, fresh v2 start)
  - Binary layout v2 = 13 bytes, version byte = 2
- `ReadingTimeHistory` (`ReadingTimeHistory.h/.cpp`) — weekly/monthly/yearly buckets + 730-day heatmap bitset, X3-only (needs RTC)
- `GlobalReadingStats` (`GlobalReadingStats.h/.cpp`) — cumulative totals + embedded `ReadingTimeHistory`, persisted to `/.crosspoint/global_stats.bin`
- Pace sampling + recording wired into `EpubReaderActivity::onExit` / `pageTurn`

## Phase 2 — Stats UI ✅ DONE
- `BookStatsActivity` — per-book screen (reader menu → "Book Stats", now last in menu): total time, dated/undated split, est. remaining (progress-percent extrapolation), Timeline/Heatmap tabs
- `ReadingStatsActivity` — global screen (Home menu → "Reading Stats", shown only when `totalReadingSeconds > 0`): same tab layout, sourced from `GlobalReadingStats`
- Wired into `ActivityManager` (`HomeMenuItem::READING_STATS`, `goToReadingStats()`, `goHome()` name-mapping) + `HomeActivity` (conditional menu entry, Chart icon)
- Chart icon added to `BaseTheme`/`LyraTheme`, 14 i18n strings added to `english.yaml`
- Build verified: `pio run -e default` → SUCCESS

## Phase 2.5 — New Home Theme ✅ DONE — verified on hardware by user
New theme for `src/components/themes/` (sibling to `lyra/`, `roundedraff/`). Layout spec from user:
```
———— hero: current/most-recent book (cover + details)
              Book Name (full, multi-line)
cover                    8% | (ticker)
              [xxxxxxxxxxx|                    ] (progress bar)
                        4h30m|
              11. Chapter xxx (current chapter)
              Last read on Sun, 7 Jun 15:25 (if RTC available)
————
Next 3 recent books (compact row/list)
————
Horizontal icon-only menu: Browse Files | Recent | OPDS | File Transfer | Reading Stats | Settings
```
Name suggestion: **Vega** — brightest star in Lyra (the constellation `LyraTheme` is named for), fits
the existing naming convention as a sibling/companion theme, and "spotlight on one star" matches a
layout that puts one book front-and-center. Alternatives: `Marquee`, `Spotlight`, `Folio`.

### Implementation ✅ DONE
- `VegaTheme.h`/`.cpp` (`src/components/themes/vega/`) — `class VegaTheme : public LyraTheme`,
  overrides `drawRecentBookCover` (hero card + cached `loadHeroDetails()` self-contained EPUB I/O
  inside the existing `coverRendered` snapshot gate, "next 3" row, selection highlights) and
  `drawButtonMenu` (bottom-anchored horizontal icon row + centred label, filled-highlight selection
  since mcrosson's `GfxRenderer` has no `drawIconInverted`). `VegaMetrics::values` computes
  `homeCoverTileHeight` from shared layout constants (`kHeroPadding`/`kSectionGap`/`kNextRowHeight`)
  so metrics and draw geometry can't drift; `homeRecentBooksCount = 4`.
- `EpubReaderUtils::Progress`/`loadProgress` (`EpubReaderUtils.h`) — new shared helper mirroring
  `EpubReaderActivity::onEnter`'s `progress.bin` read-side parsing (4-or-6-byte format). No `.bak`
  fallback ported — mcrosson's `saveProgress` writes directly, no atomic-write files exist to fall
  back to.
- i18n: `STR_CONTINUE_READING`, `STR_HOME_LAST_READ_FORMAT`, `STR_NO_OPEN_BOOK`, `STR_THEME_VEGA`
  added to `english.yaml` (other locales auto-fallback to English via `gen_i18n.py`).
- Wired into theme system: `CrossPointSettings::UI_THEME::VEGA = 4`, `SettingsList.h` theme-picker
  options, `UITheme.cpp` include + `setTheme()` case (`make_unique<VegaTheme>()` /
  `&VegaMetrics::values`).

### Post-implementation fixes ✅ DONE (found + fixed after first build-out)
- **"Next 3" cover darkening** — thumbnails were drawn downscaled from a larger cached size;
  `GfxRenderer::drawBitmap`'s nearest-neighbour downscale collapses pre-dithered 1-bit pixels via
  OR-only-dark BW compositing (~50%-dithered source -> ~75% black at 2-into-1 collapse). Fix:
  `VegaMetrics` now draws "next 3" thumbnails at **native** `homeCoverHeight` — exactly the
  cached-thumbnail generation size (`UITheme::getCoverThumbPath`/`Epub::generateThumbBmp`) — so
  `fitScale` lands at `1.0` (no downscale, no darkening). 140x226 native, `homeCoverTileHeight`
  544, cover-buffer ~31.9 KB.
- **Negative-crop OOB read/write in `GfxRenderer::drawBitmap`** (`lib/GfxRenderer/GfxRenderer.cpp`,
  pre-existing — also present in shipped `Lyra3CoversTheme`/`SleepActivity`, not Vega-only): when
  a tile is proportionally wider/taller than its source bitmap, callers' `crop = 1 -
  tileRatio/sourceRatio` goes negative, driving `cropPixX`/`cropPixY` negative and walking
  `outputRow[]` out of its `malloc`'d bounds. Fixed at the shared call site —
  `std::max(0.0f, cropX/cropY)` clamp ("no crop" instead of OOB) — closes the hole for all three
  callers (Vega, Lyra3Covers, SleepActivity) in one edit.
- **Hero progress-bar label tracks the fill position** — "`NN% - <duration>`" combined onto one
  line, right-aligned over the bar's current fill edge (`fillEdgeX`, computed once and shared with
  the fill draw so they can't drift), like a tooltip following a slider thumb; clamped to
  `[textX, textX+textW]` so it stays fully on-screen near 0%/100%.
- **Title can no longer push the detail block past the cover** — `detailBlockH` summed from the
  *actual* optional elements present (progress/bar/duration/chapter/last-read);
  `titleMaxLines = min(staticCeiling, (coverH - gap - detailBlockH) / titleLineH)` guarantees
  `title + gap + details <= coverH`. Title font `UI_12_FONT_ID -> UI_10_FONT_ID` (more chars/line).
- **"Last read on ..." now respects the user's UTC offset** — `lastReadDayIndex/Hour/Minute` are
  raw RTC reads with no offset applied (matches the status-bar clock convention);
  `formatLastRead` (`VegaTheme.cpp:52`) applies `SETTINGS.clockUtcOffsetQ` at *display* time with
  the same offset+rollover arithmetic as `HalClock::formatTime`/`formatDate` — including the
  `>104` corrupted-value clamp — shifting the day-index by +-1 on a midnight crossing and
  re-deriving date/weekday via the proleptic-calendar helpers (`readingHistoryDayIndex` family,
  leap-year safe by construction). Verified across the full `[-12:00,+14:00]` offset range.

### Design pass — resolved ✅

- **Base theme: `LyraTheme`** (`class VegaTheme : public LyraTheme`). Same convention `Lyra3CoversTheme`
  already uses (`Lyra3CoversTheme.h:18`) — inherit Lyra's header/tabs/list/keyboard styling for free
  (Vega is its sibling per the naming rationale), override only `drawRecentBookCover` (hero card +
  "next 3" row) and `drawButtonMenu` (horizontal icon row), plus its own `ThemeMetrics`
  (bigger `homeCoverTileHeight` for the extra text lines, `homeRecentBooksCount = 4`).
- **Horizontal icon menu needs zero `HomeActivity::render` changes.** It already calls
  `GUI.drawButtonMenu(...)` polymorphically (`HomeActivity.cpp:317`) — `BaseTheme::drawButtonMenu` is
  virtual and CrossInk's `LyraCarouselTheme::drawButtonMenu`
  (`uxjulia/CrossInk/.../LyraCarouselTheme.cpp:455`) overrides that *exact* signature with a
  bottom-anchored icon row + centred label, ignoring the passed-in `rect` the same way Vega can.
  Directly portable pattern, no shared-file edits.
- **Hero-card data scope: hero book only, EPUB-only for progress/chapter.** `drawRecentBookCover`
  already receives the full `recentBooks` vector + `selectorIndex` (`BaseTheme.h` signature) — Vega's
  override can draw "next 3" straight from that, no signature change. But progress%/chapter/last-read
  need real I/O (open epub, read `progress.bin`/`stats.bin`), too costly to do every render or for 4
  books — so `HomeActivity` gains a small cached `loadHeroBookDetails()` (new members:
  `heroProgressPercent`, `heroChapterTitle`, `heroLastRead*`, keyed by the hero's `path` so it only
  re-runs when the hero book changes). Progress %/chapter only resolve for EPUBs (XTC/TXT use a
  different progress format — no portable `calculateProgress`/TOC); "Last read on ..." is
  format-agnostic (`BookReadingStats::load(cachePath)` works for any reader). Hero card omits
  whichever pieces aren't available for the current book/device — same "if available" conditionality
  the user's own layout spec already calls for ("if RTC available").

Hero-card data sourcing is now resolved:
- **Progress % and chapter title** — derive on demand from the *existing* `progress.bin` (no format
  change): open the epub metadata-only via `epub.load(false, true)` (the same lightweight call
  `loadRecentCovers` already makes for thumbnails — `HomeActivity.cpp`), then
  `epub.calculateProgress(spineIndex, chapterProgress)` for `%` and
  `epub.getTocItem(epub.getTocIndexForSpineIndex(spineIndex)).title` for the chapter name. Directly
  ported from CrossInk's `RecentBookProgress::loadEpubProgressPercent`
  (`uxjulia/CrossInk/.../RecentBookProgress.cpp`) — same 6-byte `progress.bin` layout in both repos.
- **"Last read on ..."** — genuinely new persistence (nothing stores it today). Full design below.

**CrossInk reference material** (mcrosson has neither theme — would need porting/adapting, not present upstream here):
- `MinimalTheme` (`uxjulia/CrossInk/src/components/themes/minimal/MinimalTheme.cpp`) — `drawProgressBlock`
  (line 257) + `drawRecentBookCover` (line 627): cover + total-duration text + dithered progress bar +
  `"NN%"` label. Close match for the "8% | bar | 4h30m" row. Missing: chapter line, "Last read on ..." line
  — net new, would source from `BookReadingStats` / `HalClock::formatDate`.
- `LyraCarouselTheme` (`uxjulia/CrossInk/src/components/themes/lyra/LyraCarouselTheme.cpp:455`) —
  `drawButtonMenu`, commented "Horizontal icon-only menu row — anchored to bottom of screen": icon row +
  selection highlight + centered label. Exact match for the "icons only" horizontal menu requirement.
  Its carousel side-covers are "center + 2 peeks," not quite the "next 3 books" list — would need adapting.

### "Last read on ..." — persistence ✅ DONE (UI line itself lands with Vega)

Implemented per the design below: `BookReadingStats` bumped to v3 (`BookReadingStats.h` —
`lastReadDayIndex`/`lastReadHour`/`lastReadMinute`; `BookReadingStats.cpp` —
`STATS_FILE_VERSION = 3`, `STATS_FILE_SIZE = 19`, load/save read & write the new fields).
`recordReadingSession()` (`EpubReaderActivity.cpp:175`) now takes `hour`/`minute`, stamps
`lastRead*` only `if (dated)`; the call site (`EpubReaderActivity.cpp:323-328`) adds one
`halClock.getTime()` after the existing `halClock.getDate()`, gated the same way. X4 / no-RTC
naturally leaves the v3 fields at their `0` sentinel forever. Hero-card UI line ✅ DONE — lands
as part of the Vega theme build-out (`VegaTheme.cpp::formatLastRead`), reads these fields via
`readingHistoryDateFromDayIndex`/`readingHistoryDayOfWeek` per the "UI" note below, and applies
`SETTINGS.clockUtcOffsetQ` at display time (raw fields stay offset-free on disk) — see
"Post-implementation fixes" above.

Original design notes (kept for reference):

**Lives in `BookReadingStats`/`stats.bin`, not `progress.bin`** — bumps its binary layout v2 → v3:
- New fields: `uint32_t lastReadDayIndex` (day-index, 2000-01-01 = 0 — *same encoding* as
  `ReadingTimeHistory::heatmapAnchorDay`, reusing the already-written `readingHistoryDayIndex` /
  `readingHistoryDateFromDayIndex` / `readingHistoryDayOfWeek` helpers), `uint8_t lastReadHour`,
  `uint8_t lastReadMinute`. +6 bytes → 19 bytes total (`BookReadingStats.cpp:17` `STATS_FILE_SIZE`),
  version byte 2 → 3 (`STATS_FILE_VERSION`, `BookReadingStats.cpp:17-19`).
- Sentinel `lastReadDayIndex == 0` = "never recorded" — mirrors the `heatmapAnchorDay == 0` /
  `hasAnyData()` convention `ReadingTimeHistory` already uses (`ReadingTimeHistory.h:60`). Hero card
  omits the "Last read on ..." line entirely when the sentinel is set — same fallback shape as the
  existing `tr(STR_STATS_X3_ONLY)` hint in `BookStatsActivity`/`ReadingStatsActivity`.
- v2 → v3 migration: identical to the v1 → v2 jump already documented at `BookReadingStats.cpp:17-19`
  — old files fail the version check in `load()`, fresh v3 stats start with sentinel fields. Nothing
  to migrate, no new code path.

Why `stats.bin` and not `progress.bin`:
- `progress.bin` is the hot-path position-resume cache: written every page turn
  (`EpubReaderActivity.cpp:1302`), currently undocumented and has no version byte at all — extending
  it risks the resume-position path and bakes a new field into an already ad-hoc format.
- `stats.bin` already has the right shape for this: a clean version-byte / reject-on-mismatch
  convention purpose-built for schema growth, written once per session at reader exit (not per page
  turn — naturally satisfies CLAUDE.md's "debounce persistent writes"), and it groups "last read"
  with exactly the data the user wants KOReader-synced later (Phase 3) — same file, same lifecycle,
  same future payload.

**Write path — costs one new RTC read per session, not per page turn:**
`recordReadingSession()` (`EpubReaderActivity.cpp:175`) is called once at session-exit
(`EpubReaderActivity.cpp:316`) and already receives `dated`/`year`/`month`/`day`/`dayOfWeek`, computed
just above it from a single `halClock.getDate()` call (`EpubReaderActivity.cpp:313-315`). Add one
`halClock.getTime(hour, minute)` alongside it — reached only when `dated` is true (X3 + RTC available
this session) — and thread `hour`/`minute` through. Inside `recordReadingSession`, set
`bookStats.lastReadDayIndex/.lastReadHour/.lastReadMinute` *only* `if (dated)`. An undated session
(X4, or RTC momentarily unavailable on X3) leaves the previous dated "last read" stamp untouched
rather than clobbering it with a sentinel — the same "don't let an undated session erase known-good
dated history" spirit `unattributedSeconds` already follows in that function.

**X4 / no-RTC handling:** `halClock.isAvailable()` is false on X4 ⇒ `dated` is always false ⇒ the new
fields stay at their sentinel forever ⇒ the hero card never renders the "Last read on ..." line on
that device. No special-casing beyond the `if (dated)` guard above — it's the exact gate
`BookStatsActivity`/`ReadingStatsActivity` already use to show/hide `tr(STR_STATS_X3_ONLY)`.

**KOReader Sync (Phase 3) compatibility — checked, no conflict:**
`KOReaderProgress::timestamp` (`lib/KOReaderSync/KOReaderSyncClient.h:13`) is `int64_t` Unix-epoch
seconds — but it is **server-assigned**: the firmware only ever reads it back for display
(`KOReaderSyncClient.cpp:177`, `outProgress.timestamp = doc["timestamp"]`) and never constructs or
sends one (`KOReaderSyncActivity.cpp:221-226` builds the outgoing `KOReaderProgress` without touching
`.timestamp`; `KOReaderSyncClient.cpp:208-209` never serialises it into the PUT body). So there is no
existing firmware-side epoch convention to match or risk duplicating. Staying with the local
day-index + hour/minute encoding is the right call — it gets day-of-week/date formatting for free
from helpers already written this session, and stays consistent with `ReadingTimeHistory`. *If* a
future Phase 3 stats-sync payload ever needs Unix-epoch (e.g. cross-device timestamp merge), that's
an isolated one-way `dayIndex/hour/minute → epoch` conversion at the sync boundary — it neither
constrains nor is constrained by this local format choice.

**UI — hero card "Last read on Sun, 7 Jun 15:25":** format with
`readingHistoryDateFromDayIndex(lastReadDayIndex, y, m, d)` + `readingHistoryDayOfWeek(lastReadDayIndex)`
+ the existing `monthAbbr()` table — the identical helper chain the heatmap already uses
(`ReadingStatsActivity.cpp:279-296`), just assembled into a sentence instead of a grid label/tick.

## Phase 3 — KOReader Sync ⏳ PAUSED — NOT YET PLANNED IN DETAIL
Old sketch (pre-v2) is **stale** — it referenced dropped fields (`sessionCount`, `totalPagesTurned`,
Lua `stats_pages_turned_field`/`stats_avg_pace_field`) and assumed a flat "5 scalar fields, max-wins merge"
shape. Needs a fresh design pass before implementation. Open questions:

1. **Payload shape** (drives everything else):
   - Option A — scalars only (`totalReadingSeconds`, `unattributedSeconds`): simple max-wins merge, small payload, loses per-day history from other devices.
   - Option B — full `ReadingTimeHistory` (date-keyed weekly/monthly/yearly + heatmap bitset, ~785 bytes): richer, but needs date-bucket-aware merge (union heatmap bits, sum-or-max per matching week/month/year), bigger payload over HTTP.
2. **Double-counting across devices** — old plan: per-device storage on server keyed by user+device, to avoid two devices' session-seconds summing into one inflated global total. Still the right instinct; merge algorithm depends on #1.
3. **Server side** (`koreader-sync-server`, Lua):
   - `config/routes.lua` — e.g. `GET/PUT /syncs/stats/:document` (or `/api/stats/:md5`)
   - `syncs_controller.lua` — `get_stats()` / `update_stats()` actions, `stats_key` constant, Redis (or storage) field list rewritten against v2 schema
   - `syncs_controller_spec.lua` — test cases
3. **Firmware side**:
   - `KOReaderSyncClient` — `ReadingStatsPayload` struct + `getStats()`/`updateStats()`, same HTTP pattern as `getBookmarks`/`updateBookmarks`
   - `KOReaderSyncActivity` — add stats sync step *after* bookmark sync: GET → merge → PUT, **silent failure** (never blocks progress sync)
   - i18n — sync step label string

**Decision needed before coding:** settle #1 (payload shape) first — it cascades into merge algorithm (#2), server schema (#3), and client struct shape (#4).

## Phase 4 — Housekeeping
- CHANGELOG entries — **N/A for this repo**: `mcrosson-crosspoint-reader` has no `CHANGELOG.md` and its `CLAUDE.md` doesn't require one (that convention is `uxjulia/CrossInk`-specific).

## Suggested order
```
Phase 1 → Phase 2 → Phase 2.5 → Phase 3 → Phase 4
```
Phase 1 is the dependency for everything. Phase 1, 2 & 2.5 complete (incl. post-implementation
fixes — darkening, OOB-crop clamp, progress-bar tracking label, title-overflow guard, UTC-aware
"Last read on ..."); **verified on hardware by user — Phase 2.5 closed**.
Phase 3 (KOReader Sync) paused — needs a re-plan against the v3 `BookReadingStats` schema (the v2
references in its section above are now stale) before implementation.
