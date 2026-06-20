# CJK Support — Feasibility Study

Date: 2026-06-03 — **Re-verified 2026-06-17 (see "Re-verification" section at end)**
Target branch: `mcrosson-crosspoint-reader @ feat-dictionary`
Source of CJK work: `leecming82/crosspoint-reader @ feature/japanese-support`

## Goal
Port CJK (Japanese-first) text rendering onto `feat-dictionary`.

## Repos
- **mcrosson `feat-dictionary`** — your branch. Has Latin dictionary feature + image rendering. No CJK. `StringUtils.cpp` = 46 lines.
- **leecming82 `feature/japanese-support`** — CJK rendering, 22 commits over an older fork point (merge-base `20fee843`).
- **leecming82 `feature/japanese-dictionary`** — JP dictionary (deinflection/kana lookup/popup) stacked on japanese-support. SEPARATE feature.
- **leecming82 `japanese-dictionary-fusion`** — both merged.

## Scope decision
CJK support = 4 real workstreams below. leecming's image-converter commits (`a71ef29`, `ee3b25a`, `ee...`) are NOT needed — mcrosson already has image rendering (DirectPixelWriter, PixelCache, Jpeg/Png converters present).

JP *dictionary* is out of scope for "CJK support" — separate ~1-2 week project. Your `feat-dictionary` is a different (Latin) dictionary; fusing JP dict is its own effort.

## Core CJK commits (leecming, oldest→newest)
```
9b9eaa4 fallbacks for CJK text in system UI
c1deb9f Add basic CJK line breaking logic
74826e1 Handle ruby
b5b82ff Allow some CSS / f211b5f Disable CSS handling + bad_alloc guards
7e95c6a Fixes for ascii overlapping
723d8c9 clear font memory cache OnExit (inter-book stalls)
33755fb Tweak fallback styles so prewarm cache falls back
1680387 Synthetic bold
b5c89b5 Ignore unnecessary kobo ebook anchors
c210825 Bug fix for dangling punctuation
10c996a Made cjk parsing more robust
34b359e Fix slow page flips after indexing (clear memory)
27e400a Expand CJK unicode interval (ascii, punct, box drawing)
d29e3d0 more character layout fixes
ea33da6 Temp fix for Japanese filenames
```

## File map + divergence (mcrosson base vs leecming pre-CJK base)
Lower base divergence = cleaner port.

| Area | File | leecming add | base gap | Risk |
|---|---|---|---|---|
| CJK line-break layout (kinsoku, hanging punct, chunked layout, heap guards) | `lib/Epub/Epub/ParsedText.cpp` | +758 | **16 lines** | LOW |
| Ruby `<ruby>/<rt>` parse + utf8-safe word buffer | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | +214 | 204 | MED |
| Font fallback face resolve | `lib/EpdFont/SdCardFont.cpp`, `EpdFontFamily.*`, `FontCacheManager.*` | ~75 | 206 (fork noise; fns additive) | LOW-MED |
| Synthetic-bold glyph render | `lib/GfxRenderer/GfxRenderer.cpp` | +26 | 457 (noise NOT in render path) | LOW |
| UI fallback markers (`JP[..]`/`CJK[..]`) | `src/util/StringUtils.cpp` | +136 | new | LOW |
| Activity wiring (CJK wrapper toggle, JA orientation) | `EpubReaderActivity`, Home/FileBrowser, themes | ~100 | varies | LOW-MED |
| Font asset/build | `lib/EpdFont/scripts/fontconvert_sdcard.py` (`"cjk"` charset interval) + ship CJK glyph font to SD | +3 + asset | — | external |

### Key verification (de-risked GfxRenderer)
mcrosson `GfxRenderer.cpp` has IDENTICAL render hook points:
- `renderCharImpl<TextRotation::None>` at ~line 473 / 505
- `renderCharImpl<TextRotation::Rotated90CW>` at ~line 1654

Synthetic-bold patch = thin wrapper `renderCharWithSyntheticBold<rotation>(...)` that draws glyph 3× (x, x+1, x+2) + `const bool syntheticBold = isSdCardFont(fontId) && font.needsSyntheticBold(style);`. Swap into 4 call sites + 1 helper. The 457-line "divergence" is fork-age noise elsewhere, not the render path → risk drops HIGH→LOW.

### CJK detection (from leecming StringUtils / ParsedText)
```
isCjkCodepoint: 0x3000-303F, 0x3040-30FF, 0x31F0-31FF, 0x3400-4DBF,
                0x4E00-9FFF, 0xF900-FAFF, 0xFF00-FFEF, 0x20000-2FA1F
isJapaneseKana: 0x3040-30FF, 0x31F0-31FF, 0xFF66-9F
```
Layout uses kinsoku rules: `isCjkNoLineStart`, `isCjkNoLineEnd`, opening/closing punctuation, hanging closing punct, chunk boundaries.

### Heap guards (DO NOT SKIP — ESP32)
```
MIN_FREE_HEAP_FOR_CJK_LAYOUT = 48 * 1024
MAX_CJK_LAYOUT_UNITS = 1024
CJK_LAYOUT_CHUNK_TARGET_UNITS = 768
+ clear font memory cache OnExit, clear memory after indexing
```

## Effort
| Workstream | Effort | Success |
|---|---|---|
| StringUtils CJK helpers | ½ day | ~98% |
| ParsedText CJK layout | 1-2 days | ~90% |
| Font fallback (SdCardFont/EpdFontFamily/FontCacheManager) | 1 day | ~90% |
| GfxRenderer synthetic bold | ½ day | ~90% |
| Ruby parser merge | 1 day | ~75% (optional) |
| Wiring + themes | 1 day | ~85% |
| CJK font asset + fontconvert | ½-1 day | ~80% |

**Core CJK total: ~5-7 days.**

## Success odds
- JP/CJK text displays + wraps (kinsoku): **~85%** — main path low-risk, patches surgical, hooks match.
- Ruby/furigana correct: **~70%** — optional, ship without first.
- Stable on device (ESP32 heap): **~70%** — real risk is OOM / slow page flips, not compile. Port heap guards.
- Font asset fits: **~80%** — full CJK is big; likely need JP subset (Joyo + kana ≈ 2-3k glyphs vs 20k+ full CJK).

**Overall:** high confidence on hard-looking parts (render/layout — structure matches). Residual risk is RUNTIME (device memory + font size), tunable not blocking.

## Recommended build order (each step renders something testable)
1. StringUtils helpers
2. Font fallback + GfxRenderer synthetic-bold wrapper
3. ParsedText CJK layout (+ heap guards)
4. CJK font asset (now JP text renders end-to-end)
5. Ruby parser
6. Wiring + theme polish

## Open questions before committing days
1. Device target (X3/X4/T5) → free flash/heap → dictates font subset.
2. Furigana (ruby) needed at launch?
3. JP dictionary wanted later? (separate project, fuse `feature/japanese-dictionary`)

## Quick re-derive commands
```
cd leecming82/crosspoint-reader
MB=20fee843c721e5392c1854bee250538153d92860
git log --oneline origin/master..origin/feature/japanese-support
git diff --stat $MB origin/feature/japanese-support
# cross-fork base divergence:
git -C leecming82/crosspoint-reader show $MB:<file> | diff - mcrosson-crosspoint-reader/<file>
```

---

## Re-verification (2026-06-17)

Re-derived current state of both repos. Original 4-workstream structure holds. One material delta: **mcrosson's SD-font subsystem evolved past leecming's base**, pushing the font workstreams up in risk.

### Confirmed unchanged
- mcrosson `feat-dictionary`: `src/util/StringUtils.cpp` still **46 lines, zero CJK**. Clean slate.
- leecming `feature/japanese-support`: still 22 commits over merge-base `20fee843`. Core commits intact.
- GfxRenderer render hooks still match (`mcrosson GfxRenderer.cpp` now 1978 lines) → synthetic-bold port stays LOW risk.
- `ParsedText.cpp` = 969 lines (base for kinsoku port).
- Target hardware confirmed `esp32-c3-devkitm-1` = **ESP32-C3, 380KB RAM, NO PSRAM, single 48KB framebuffer.** Runtime OOM remains the dominant (non-code) risk.

### `m4-japanese-port` branch — NOT relevant
leecming has a `m4-japanese-port` branch (also `japanese-dictionary-fusion`). `m4-japanese-port` = leecming's own port to a *different* "M4" hardware (external RTC, SD_MMC backend, +4K Arduino stack), stacked on `japanese-dictionary-fusion` (japanese-support + JP dictionary). **Not** mcrosson's fork — do not cherry-pick from it. Value = proof CJK + JP-dict run together on a real e-reader device.

### Delta: font subsystem diverged (risk UP)
mcrosson now ships a rewritten SD-font stack:
```
SdCardFont + SdCardFontManager + SdCardFontRegistry + EpdFontFamily + FontDecompressor(hot-group on-demand decompress)
```
leecming's CJK font-fallback patch (`+75 lines`) was written against the **older** `SdCardFont`; `SdCardFontManager`/`Registry`/`FontDecompressor` did NOT exist at leecming base. Consequence:
- **Font-fallback workstream: LOW-MED → MED.** Cannot cherry-pick clean; must re-implement fallback face-resolve against mcrosson's manager/registry API.
- `lib/EpdFont/scripts/fontconvert_sdcard.py` (the `"cjk"` charset script the original doc referenced) is **gone** from mcrosson. mcrosson uses different font tooling → CJK glyph-asset generation must be ported to mcrosson's format. Asset workstream +½ day, MED.

### Delta: good news
mcrosson's `FontDecompressor` already does **on-demand glyph decompression from SD** (hot-group + fallback path). That is exactly the mechanism needed to fit CJK without flashing 20k glyphs — infra already present, currently wired for Latin only.

### Revised effort
| Workstream | Effort | Risk |
|---|---|---|
| StringUtils CJK helpers | ½ d | LOW |
| ParsedText kinsoku layout + heap guards | 1-2 d | LOW-MED |
| Font fallback (re-impl vs new SdCardFontManager API) | **1-1.5 d** ↑ | **MED** ↑ |
| GfxRenderer synthetic-bold | ½ d | LOW |
| CJK font asset (port to mcrosson font tooling) | **1 d** ↑ | MED |
| Ruby/furigana | 1 d | MED (skip v1) |
| Wiring + themes | 1 d | LOW-MED |

**Core text-display total: ~6-8 days** (was 5-7; font-subsystem drift +1d).

### Revised odds
- JP text displays + wraps (kinsoku): **~85%** (render/layout path low-risk, hooks match)
- Stable on ESP32-C3 device: **~70%** (380KB RAM / no PSRAM ceiling; mitigable via JP subset + heap guards)
- Font asset fits flash/heap: **~80%** (JP subset required: Joyo + kana ≈ 2-3k glyphs vs 20k+ full CJK)

### Biggest difficulty
Not the layout/render code (surgical, hooks match). It is **(1)** re-fitting leecming's font fallback onto mcrosson's newer `SdCardFontManager` API, and **(2)** keeping the CJK glyph working set under the 380KB RAM ceiling on a no-PSRAM C3.

### Open decisions before committing days
1. JP glyph subset size (= device flash/heap budget).
2. Furigana (ruby) at launch? Skip → save 1d + a MED-risk workstream.
3. JP dictionary later? Separate project (fuse `feature/japanese-dictionary`).
