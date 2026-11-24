# matcha-reader sync ledger

Vertical text (tategaki), the furigana toggle and per-book Book Language are ported from
[matcha-reader](https://github.com/eszter007/matcha-reader). This file records what was taken, how
it was adapted, and where the next sync starts.

**Last synced matcha commit:** `13f08dee` (2026-11-17).

## Syncing

```bash
cd ../matcha-reader && git pull && cd -
scripts/matcha_sync_status.sh            # report only
scripts/matcha_sync_status.sh --merge    # also three-way merge every changed ported file
```

The report sorts every ported file into UNCHANGED, TAKE (ours equals matcha at the last sync:
take matcha's file), MERGE (ours has marked edits: three-way merge) and GONE. It then lists
headers that matcha's new code includes and we lack (NEW?: copy it if it is engine code, shim it
if it is matcha UI), and diffs the reader-glue functions below in matcha's `EpubReaderActivity.cpp`.

Steps, in this order:

1. **Read the commit list first** and sort each commit: engine (take), reader glue (hand-port),
   or matcha-only UI (skip, add to *Not ported*). Lookup and vertical are coupled
   (`WordSelectionScan` uses `VerticalClipCells` and the vertical page API), so take them together.
2. **Merge, then resolve conflicts.** A conflict in a file that matcha restructured (the 11-17
   sync split `VerticalParsedText.cpp` into `VerticalLayout.cpp`) is cheaper resolved by taking
   matcha's file whole and re-applying our marked edits from the table below.
3. **Re-check every `companion:` / `crosspoint:` edit** in the tables still applies: matcha may
   have fixed the same thing (the na-particle split and the page-buffer recycling both arrived
   upstream), and then ours goes.
4. **Build `default` and `x4pro`**: PSRAM-only code (`#ifdef BOARD_HAS_PSRAM`) compiles only on x4pro.
5. **Tests:** rebuild before trusting ctest (`cmake --build build/test` then `ctest`: a stale
   binary reports green), plus the four `run.sh` suites. Copy matcha's new host tests for engine
   files when they build against our stubs.
6. **Glue the report lists:** port each changed function by hand into `EpubReaderCjk.cpp` /
   `EpubReaderVertical.cpp` / `DictionaryWordSelectScan.cpp`.
7. **Translation** goes through `scripts/sync_matcha_translate.sh`; keep our side on conflicts
   (model chain, Retry, touch zones) and hand-port only what we want.
8. **Bump `LAST_SYNC`** in both scripts and the hash above; record skipped features under *Not ported*.

Keep `VSECTION_FILE_VERSION` equal to matcha's: a fix that changes the vertical cache format
brings its bump with it. A change to the `cjk.bin` sniff bumps `CjkBookPrefs::SNIFF_VERSION`.

Dictionary data: matcha's `tools/dict_convert` output feeds both `/dictionaries/<lang>` and our
`scripts/matcha_dict_to_stardict.py`. When matcha changes the definition text format, update the
converter (it already strips the `｛furigana｝` matcha appends to reading lines).

## Known gaps (decided, revisit when wanted)

- **Vertical CSS font-size**: needs a scaled glyph path in `GfxRenderer` (matcha's
  `drawTextScaled` / `layoutTextScaled` / `FaceScale`). With one, drop the
  `verticalBlockStyleOf` font-size omission and the pass-through `scale` parameters.
- **Background lookup prescan** (matcha `b7b7b7b0`, `stepLookupPrescan`): PSRAM boards only (X4
  Pro). Scans the shown page into `wlscan.bin` after 1.5 s so word-select opens without a scan.
  Porting needs the prescan to build its scan exactly as `DictionaryWordSelectScan` does (same
  page, tail and glossary), or the cache key will not match.
- **Tategaki / Zhipai settings** (matcha `useVerticalText`): global per-script vertical
  defaults. We keep the per-book Auto/On/Off in `cjk.bin`.
- **Embedded Style off** does not reach vertical text (neither in matcha): book CSS and chapter
  `<style>` blocks still style vertical chapters.
- **Gemini key/model page** (`GeminiConfig`) and **PanelInput** are matcha UI; the report keeps
  listing them as NEW? until ported.

## Verbatim files (byte-identical to matcha)

| Path | Role |
|------|------|
| `lib/Epub/Epub/VerticalSection.{h,cpp}` | Vertical chapter cache: build, slices, page/offset/anchor lookup |
| `lib/Epub/Epub/VerticalParsedText.{h,cpp}`, `VerticalLayout.cpp`, `VerticalLayoutSupport.h` | Column layout engine |
| `lib/Epub/Epub/VerticalBatch.h` | Batch sizes; larger and in PSRAM on PSRAM boards |
| `lib/Epub/Epub/BookFontMap.{h,cpp}` | Publisher-font map; we install none, so every family is the reader font |
| `lib/XmlParserUtils/GaijiText.h` | Gaiji image replacement text (moved out of XmlParserUtils.h) |
| `lib/Memory/ResidentFiles.h` | Dictionary indexes kept in PSRAM (X4 Pro); no-op without PSRAM |
| `lib/Epub/Epub/blocks/VerticalTextBlock.{h,cpp}` | Draws a vertical page |
| `lib/Epub/Epub/Kinsoku.{h,cpp}` | Japanese/Chinese line-breaking and punctuation rules |
| `lib/Epub/Epub/RubyGlossary.{h,cpp}` | Ruby harvest used by the vertical build |
| `lib/Epub/Epub/parsers/XhtmlDoctype.h` | XHTML doctype handling for the vertical parser |
| `lib/Epub/Epub/PageTextExtractor.{h,cpp}` | Page text for glyph prewarm and bookmark snippets |
| `lib/Epub/Epub/VisibleTextUtils.h` | Shared visible-text rules (superset of ours) |
| `lib/XmlParserUtils/XmlParserUtils.h` | Adds `gaijiReplacementText` (superset of ours) |
| `src/util/CjkScript.h` | Language tag -> script, language choices, content sniff |
| `src/activities/reader/VerticalClipCells.{h,cpp}` | Per-cell screen rects of a vertical page |
| `lib/Dict/{DictIndex,Deinflector,WordLookup,CounterReading}.{h,cpp}` | Converted dictionary (`/dictionaries/<lang>/vocab.*`), deinflection, longest-match lookup |
| `src/activities/reader/WordSelectionScan.h` | Resumable page segmenter (whole-word selection) |
| `test/cjk_script/CjkScriptTest.cpp` | Host tests for `CjkScript.h` |
| `test/{counter_reading,word_lookup_*}/`, `test/common/dict_stubs/` | Host tests for the lookup engine (matcha's `ruby_glossary_prefix` needs stubs we lack) |

## Edited copies (`// companion:` marks every edit)

| Path | Edit |
|------|------|
| `lib/Serialization/Serialization.h` | `readPod` zeroes with `memset`, not `T{}`, so arrays of PODs (reading-time history) still compile |
| `src/activities/reader/WordSelectionScan.cpp` | `measurePage` reads our `characterSpacing` for matcha's `letterSpacing` |
| `src/activities/reader/WordSelectionScan.{h,cpp}` | Heap stall/retry in `pushSelectable` (X3); guarded index growth in `tryLoadCache`; `SdDebugLog` lines |
| `lib/Dict/DictIndex.{h,cpp}` | `setLowMemory` / `cachesStarved` (X3 gloss box) |
| `lib/Epub/Epub/VerticalLayout.cpp` | Glyph text dropped, not aborted, when its pool cannot grow |
| `lib/Epub/Epub/VerticalSection.{h,cpp}` | `lostTextNeedsRebuild()` / `lastBuildLostText_`, used by `rebuildVerticalAfterLostText` |
| `lib/Epub/Epub/blocks/VerticalTextBlock.{h,cpp}` | `rubyBottomY`: a long reading at a column foot ends above the status bar |
| `lib/Memory/ResidentFiles.cpp` | `#include <SdFat.h>` so the x4pro build finds SdFat |
| `lib/hal/SdSystemDir.cpp` | `/.crosspoint` as the folder (translation sync) |

## Shims on our side (matcha's names, our implementation)

| Where | What |
|-------|------|
| `GfxRenderer` | `drawTextRotated90CCW`, `getRenderAdvanceX`, `getGlyphMetrics`, `textBaselineOffset`, `drawCharVerticalCornerTopRight`, `drawCharVerticalRotatedInCell`, `verticalPunctInkBox`; `TextRotation::Rotated90CCW` in `renderCharImpl` (glyph frame) and `renderCharScaled` |
| `FontDecompressor::getStarvedGlyphCount()` | Counts hot-group OOM skips; never reset |
| `FontCacheManager::releaseAllFontMemory()` | Alias of our `releaseCache()` |
| `ImageBlock::fitWithin` / `fillsPage` | Same bodies as matcha; images draw upright (matcha's rotated full-page images are not ported) |
| `Epub::readItemContentsToStream(..., shouldCancel, ctx)` | Cancellable overload via a forwarding `Print` |
| `Epub::stylesheetFilterFor`, `CssParser::StylesheetFilter` | Admit every rule: our CSS cache does not record rule sources |
| `CssParser::collectVerticalStyles` | Reads OUR cache record format into a `CssStyle`, then `verticalBlockStyleOf` |
| `CssParser::verticalBlockStyleOf` / `appendChapterVerticalStyles` | Margins, hanging and first-line indent, centre/end alignment, borders; a chapter's own `<style>` blocks. Never sets `fontScale32` or `blockAlign` (no `auto` unit) |
| `ReaderFontScale.h` | `fontScaleTagForScale` and two helpers only; our horizontal sizing is `applyBlockFontScale` |
| `GfxRenderer::drawTextScaled` / `drawTextRotated90CCWScaled`, `scale` on the vertical-cell functions | Pass-throughs: no scaled glyph path, and vertical text is always at scale 256 |
| `ReaderRenderSpec::bookFontsSignature` | Always 0 |
| `ReaderRenderSpec` | `furiganaEnabled`, `lineSpacingLevel`, `rightMarginPx` |
| `BookMetadataCache` / `ContentOpfParser` / `Epub::pageProgressionRtl()` | Spine direction, book.bin v11 (same number as matcha) |
| `platformio.ini` | `-Ilib/Epub/Epub`, as matcha builds |
| `TextBlock::wordPlacements` / `WordPlacement` | For `WordSelectionScan::measurePage`, which we never call (no horizontal geometry: tokens map by line + codepoint) |

## Reader glue (hand-ported; matcha function names)

- `src/activities/reader/EpubReaderCjk.cpp`: `loadLanguageChoice`, `refreshBookScript`,
  `sniffLanguageIfNeeded`, `readerSpec`. Per-book state is `cjk.bin` (`CjkBookPrefs.h`), not
  matcha's `language.bin` + `progress.bin` bytes.
- `src/activities/reader/EpubReaderVertical.cpp`: `useVerticalText`, `renderVerticalPageBody`,
  `prewarmVerticalPageGlyphs`, `earlyRenderVerticalPage`, `tickVerticalBuild`, plus our own
  `renderVertical`, `presentVerticalPage`, `openVerticalWordSelect`.

- Whole-word selection: `src/activities/reader/DictionaryWordSelectScan.cpp` drives the scan with
  matcha's `EpubReaderWordLookupActivity` timing (burst around the cursor, capped 1.5 s; 40 ms
  slices while not rendering; `wlscan.bin` cache; `DictIndex::releaseCaches()` on exit).
  `src/util/CjkWordMerge.{h,cpp}` merges our per-character tokens into the scan's words; saved
  highlights stay in character indices. `EpubReaderCjk.cpp`: `newWordScan`, `wordScanLookupTail`;
  `refreshBookScript` sets `DictIndex::setLanguageFolder` as matcha's `applyLanguageState` does.
- `verticalSpec()` passes `rightMarginPx = 0` with furigana on, so the first column's ruby gutter
  sits inside the text area; `renderVertical()` lays columns out half a line short of the status bar.
- Vertical marks and highlights (not in matcha): `VerticalPageMarks.{h,cpp}` over
  `forEachVerticalToken` (`VerticalWordTokens.h`, the numbering word-select uses). Quotes saved on a
  vertical page carry the exact chapter offset of their first character.
- Dictionary tabs: `src/util/DictTabs.{h,cpp}` maps StarDict copies tagged `matchaslot=` (written
  by `scripts/matcha_dict_to_stardict.py`) to matcha's slots; `probeTabs()` asks `DictIndex`.
- 13f08dee sync: `DictionaryWordSelectScan` calls `setBookGlossary` before `tryLoadCache` (names
  the book annotates select as one word); `refreshBookScript` wraps the folder in
  `DictIndex::installedFolderFor` (Cantonese falls back to Mandarin); `initFromVerticalPage` takes
  the reader's `cellPx`.
  `sniffLanguageIfNeeded` detects Cantonese (Hong Kong tags and Cantonese particles) and
  `CjkBookPrefs` carries a sniff version (byte 5 of `cjk.bin`), so books sniffed before re-sniff once.
- Dictionary form: `DictionaryLookupController::runLookup()` probes `WordLookup`'s headword (or the
  `Deinflector`'s first 6 candidates without a converted dictionary) after an exact miss.

## Not ported

- matcha's lookup panel (`EpubReaderWordLookupActivity`), sentence mining, translation and the
  names/grammar tabs: definitions show in our StarDict screen, so a StarDict copy of the same
  dictionary is needed (`scripts/matcha_dict_to_stardict.py`).
- Built-in Noto Sans JP UI fonts (~710 KB flash) and the CJK companion font: vertical needs a
  reader font with the book's script.
- Clippings, highlights, page underlines and footnote selection on vertical pages.
- Per-chapter stylesheet filtering (matcha `7581162e`).
- Inline gloss box on vertical pages.
- CSS font-size in vertical text (`56bbd8ec`): needs matcha's scaled glyph drawing. Vertical text
  stays at the body size.
- Publisher fonts (`9fb64776`), touch-selection lookup (`a105a2d0`), lookup inside an entry
  (`fe74a97e`), the save popup (`f10dddee`), the Dictionary Settings page (`5176a13c`), the
  Gemini key/model settings page (`7ab3e9e1`; our `GEMINI_MODELS` chain stays), one input model
  for panels (`d209e69b`), and panels following Reversed Page Turn (`4cc8944e`; we have no such
  option).
- Translate To (`272ae112`) is re-implemented in `src/util/TranslationTarget.{h,cpp}` as a
  Reader > Dictionary setting; Custom reads `gemini.lang` (no on-device typing).
