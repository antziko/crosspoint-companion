# Change Summary — OPDS, Recent Books, Sleep Wallpaper

Branch: `feat-dictionary`. Three independent changes, all build clean (`pio run`).
Device verification (X3 + X4) still pending where noted.

---

## 1. Sleep wallpaper — exhaustive shuffle-bag ("deck")

**Goal:** every wallpaper in the sleep folder is shown once, in random order, before
any repeat — instead of "random with a short memory" that let some images recur while
others starved.

**Mechanism (`SleepActivity.cpp`):**
- Sort the BMP file list (`FsHelpers::sortFileList`) so a file's index is stable across
  wakes — directory iteration order is not guaranteed, and the deck stores indices.
- Track which images were shown this cycle in a persistent bitset. Pick uniformly among
  the not-yet-shown images; when the cycle is exhausted (or the folder size changes),
  start a fresh cycle. Selection is done with two counting passes — **no heap allocation**.

**State (`CrossPointState.h/.cpp`):** replaces the old 16-entry "recent" circular buffer
with a deck:
- `sleepDeckShown[64]` bitset (1 bit per image, up to `SLEEP_DECK_MAX` = 512 images),
  `sleepDeckSize`, `sleepDeckShownCount`. ~68 bytes resident (net +34 vs the old buffer).
- Helpers `isSleepShown` / `markSleepShown` / `resetSleepDeck`.

**Persistence (`JsonSettingsIO.cpp`):** deck saved in the existing `state.json` on the SD
card (same file/cadence as before — no new file, no extra writes). Old state.json without
the deck keys just starts a fresh cycle (no migration needed).

**Resource profile:** stored on SD; ~68 bytes RAM; zero heap; two O(n) bitset passes at
wake only (sleep is infrequent) → no measurable CPU/heat.

**Limit:** up to 512 images tracked per folder; beyond that, extra images aren't picked
(raise `SLEEP_DECK_MAX`, 8 bytes RAM per +64 images). Swapping files while keeping the
same count can repeat/skip until the cycle ends, then self-heals.

---

## 2. Recent Books — reorder, fixed for X3

**Goal:** reorder the Recent Books list with the front Left/Right buttons.

**Why the first attempt failed on X3:** the original used a *hold* gesture
(`isPressed` + `getHeldTime`). On X3 the front Left/Right buttons bounce into a stream of
release events when held (the cursor moved but the hold never registered), so any
hold-based scheme is unreliable there. Confirm-hold works because that ADC value is stable.

**Fix (`RecentBooksActivity.h/.cpp`):** tap-based instead of hold-based.
- **Tap Left** = move selected book up; **tap Right** = move it down (persisted per move).
- **Cursor** moves on the **Up/Down side buttons** only; Left/Right are intercepted before
  the navigator and excluded from cursor nav.
- Works identically on X3 and X4. The recent list never paginates (`MAX_RECENT_BOOKS` = 10),
  so dropping Left/Right from cursor nav costs nothing.
- Store gains `moveUp` / `moveDown` (in-memory swap); activity persists via `saveToFile`.

**Note:** opening any book still force-moves it to the front (recency model), so a manual
arrangement holds only until the next book is opened.

---

## 3. OPDS browser — alphabetical sort (toggleable)

**Goal:** show books A–Z, with an opt-out.

**Sort (`OpdsBookBrowserActivity.cpp`):** after each fetch, sort the page case-insensitively
by title, navigation folders before books, before the prev/next page links are added (so
those stay pinned top/bottom). Per-page only — OPDS feeds paginate server-side. Gated on the
new setting.

**Toggle:** new setting `opdsSortAlphabetical` (default ON).
- `CrossPointSettings.h` + `SettingsList.h`: hidden `STR_NONE_OPT` Toggle → persisted with
  all settings (and the web settings API), but kept out of the generic Settings menu.
- Surfaced/flipped in **System > OPDS Servers** (`OpdsServerListActivity.cpp`) as a bottom
  row "Sort books alphabetically" with an ON/OFF subtitle (settings mode only, not picker).
- i18n: added `STR_OPDS_SORT_ALPHABETICAL` (English; other languages fall back).

---

## Related, already committed earlier this branch
- OPDS downloaded-marker now matches existing filenames in either order
  (`Title - Author` / `Author - Title`, plus author-embedded-in-title) at `/` and `/read`.
- Investigated X3 OPDS "slow to open": SD bus is 40 MHz on both, so the delta is the X3
  e-ink refresh (16 MHz display SPI + forced full-syncs + extra settle, ×several refreshes
  during open). Diagnostic: timestamped `/opds_debug.log`. No code change yet.
