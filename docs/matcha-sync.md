# matcha-reader sync ledger

Vertical text (tategaki), the furigana toggle and per-book Book Language are ported from
[matcha-reader](https://github.com/eszter007/matcha-reader). This file records what was taken, how
it was adapted, and where the next sync starts.

**Last synced matcha commit:** `2576f466` (2026-10-14).

## Syncing

```bash
cd ../matcha-reader && git pull
git log --oneline 2576f466..HEAD -- <verbatim paths below>
```

For each commit: copy verbatim files as-is (`.clang-format` is identical in both repos), re-apply
the `// companion:` edits listed below, and hand-port any change to matcha's
`EpubReaderActivity.cpp` functions listed under *Reader glue*. Build, run `pio run -t unit-tests`,
then update the commit hash above.

Keep `VSECTION_FILE_VERSION` equal to matcha's: a fix that changes the vertical cache format
brings its bump with it.

## Verbatim files (byte-identical to matcha)

| Path | Role |
|------|------|
| `lib/Epub/Epub/VerticalSection.{h,cpp}` | Vertical chapter cache: build, slices, page/offset/anchor lookup |
| `lib/Epub/Epub/VerticalParsedText.{h,cpp}` | Column layout engine |
| `lib/Epub/Epub/blocks/VerticalTextBlock.{h,cpp}` | Draws a vertical page |
| `lib/Epub/Epub/Kinsoku.{h,cpp}` | Japanese/Chinese line-breaking and punctuation rules |
| `lib/Epub/Epub/RubyGlossary.{h,cpp}` | Ruby harvest used by the vertical build |
| `lib/Epub/Epub/parsers/XhtmlDoctype.h` | XHTML doctype handling for the vertical parser |
| `lib/Epub/Epub/PageTextExtractor.{h,cpp}` | Page text for glyph prewarm and bookmark snippets |
| `lib/Epub/Epub/VisibleTextUtils.h` | Shared visible-text rules (superset of ours) |
| `lib/XmlParserUtils/XmlParserUtils.h` | Adds `gaijiReplacementText` (superset of ours) |
| `src/util/CjkScript.h` | Language tag -> script, language choices, content sniff |
| `src/activities/reader/VerticalClipCells.{h,cpp}` | Per-cell screen rects of a vertical page |
| `lib/Dict/{DictIndex,Deinflector,WordLookup}.{h,cpp}` | Converted dictionary (`/dictionaries/<lang>/vocab.*`), deinflection, longest-match lookup |
| `src/activities/reader/WordSelectionScan.h` | Resumable page segmenter (whole-word selection) |
| `test/cjk_script/CjkScriptTest.cpp` | Host tests for `CjkScript.h` |

## Edited copies (`// companion:` marks every edit)

| Path | Edit |
|------|------|
| `lib/Serialization/Serialization.h` | `readPod` zeroes with `memset`, not `T{}`, so arrays of PODs (reading-time history) still compile |
| `src/activities/reader/WordSelectionScan.cpp` | `measurePage` reads our `characterSpacing` for matcha's `letterSpacing` |

## Shims on our side (matcha's names, our implementation)

| Where | What |
|-------|------|
| `GfxRenderer` | `drawTextRotated90CCW`, `getRenderAdvanceX`, `getGlyphMetrics`, `textBaselineOffset`, `drawCharVerticalCornerTopRight`, `drawCharVerticalRotatedInCell`, `verticalPunctInkBox`; `TextRotation::Rotated90CCW` in `renderCharImpl` (glyph frame) and `renderCharScaled` |
| `FontDecompressor::getStarvedGlyphCount()` | Counts hot-group OOM skips; never reset |
| `FontCacheManager::releaseAllFontMemory()` | Alias of our `releaseCache()` |
| `ImageBlock::fitWithin` / `fillsPage` | Same bodies as matcha; images draw upright (matcha's rotated full-page images are not ported) |
| `Epub::readItemContentsToStream(..., shouldCancel, ctx)` | Cancellable overload via a forwarding `Print` |
| `Epub::stylesheetFilterFor`, `CssParser::StylesheetFilter` | Admit every rule: our CSS cache does not record rule sources |
| `CssParser::collectVerticalStyles` | Reads OUR cache record format (v10); no border field, so `borderEdges` stays 0 |
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
