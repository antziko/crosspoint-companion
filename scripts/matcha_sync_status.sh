#!/usr/bin/env bash
# What changed in matcha-reader since the last sync, sorted the way a sync handles it.
#
#   scripts/matcha_sync_status.sh [--merge] [path/to/matcha-reader]
#
# Report (default): the commits since LAST_SYNC that touch ported files, then each ported file as
#   UNCHANGED  matcha did not touch it
#   TAKE       ours is byte-identical to matcha@LAST_SYNC: copy matcha's HEAD over it
#   MERGE      ours carries local edits (// companion: / crosspoint:): three-way merge
#   NEW?       a file matcha added that a ported file now includes (copy it, or shim it)
# then the reader-glue functions whose body changed in matcha's EpubReaderActivity.cpp.
# --merge also runs the three-way merge (git merge-file) on every TAKE/MERGE file.
# After a sync: build both envs, run the tests, set LAST_SYNC here and in
# scripts/sync_matcha_translate.sh, and update docs/matcha-sync.md.
set -euo pipefail

LAST_SYNC=13f08dee
MERGE=0
if [[ "${1:-}" == "--merge" ]]; then
  MERGE=1
  shift
fi
MATCHA=${1:-../matcha-reader}

# Engine files kept verbatim or with marked edits. Translation files live in
# sync_matcha_translate.sh and are listed here too, so one report covers both.
FILES=(
  lib/Dict/CounterReading.cpp lib/Dict/CounterReading.h
  lib/Dict/Deinflector.cpp lib/Dict/Deinflector.h
  lib/Dict/DictIndex.cpp lib/Dict/DictIndex.h
  lib/Dict/WordLookup.cpp lib/Dict/WordLookup.h
  lib/Epub/Epub/BookFontMap.cpp lib/Epub/Epub/BookFontMap.h
  lib/Epub/Epub/Kinsoku.cpp lib/Epub/Epub/Kinsoku.h
  lib/Epub/Epub/PageTextExtractor.cpp lib/Epub/Epub/PageTextExtractor.h
  lib/Epub/Epub/RubyGlossary.cpp lib/Epub/Epub/RubyGlossary.h
  lib/Epub/Epub/VerticalBatch.h
  lib/Epub/Epub/VerticalLayout.cpp lib/Epub/Epub/VerticalLayoutSupport.h
  lib/Epub/Epub/VerticalParsedText.cpp lib/Epub/Epub/VerticalParsedText.h
  lib/Epub/Epub/VerticalSection.cpp lib/Epub/Epub/VerticalSection.h
  lib/Epub/Epub/VisibleTextUtils.h
  lib/Epub/Epub/blocks/VerticalTextBlock.cpp lib/Epub/Epub/blocks/VerticalTextBlock.h
  lib/Epub/Epub/parsers/XhtmlDoctype.h
  lib/Memory/ResidentFiles.cpp lib/Memory/ResidentFiles.h
  lib/Serialization/Serialization.h
  lib/XmlParserUtils/GaijiText.h lib/XmlParserUtils/XmlParserUtils.h
  lib/hal/SdSystemDir.cpp lib/hal/SdSystemDir.h
  src/activities/reader/VerticalClipCells.cpp src/activities/reader/VerticalClipCells.h
  src/activities/reader/WordSelectionScan.cpp src/activities/reader/WordSelectionScan.h
  src/util/CjkScript.h
  test/cjk_script/CjkScriptTest.cpp
  src/activities/reader/EpubReaderTranslationActivity.cpp src/activities/reader/EpubReaderTranslationActivity.h
  src/activities/reader/PanelTouch.h
  src/components/DictionaryPanel.cpp src/components/DictionaryPanel.h
  src/components/PanelTextPages.cpp src/components/PanelTextPages.h
  src/components/icons/dictionaryIcons.h
)

# matcha's EpubReaderActivity.cpp functions our glue mirrors (see docs/matcha-sync.md).
GLUE=(useVerticalText renderVerticalPageBody prewarmVerticalPageGlyphs earlyRenderVerticalPage tickVerticalBuild
  loadLanguageChoice saveLanguageChoice applyLanguageState sniffLanguageIfNeeded effectiveLanguage readerSpec
  stepLookupPrescan)

head=$(git -C "$MATCHA" rev-parse --short HEAD)
echo "matcha $LAST_SYNC..$head"
echo
echo "== Commits touching ported files"
git -C "$MATCHA" log --no-merges --format='%h %ad %s' --date=short "$LAST_SYNC..HEAD" -- "${FILES[@]}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo
echo "== Ported files"
for f in "${FILES[@]}"; do
  if ! git -C "$MATCHA" cat-file -e "HEAD:$f" 2>/dev/null; then
    echo "GONE       $f (removed or moved in matcha)"
    continue
  fi
  if git -C "$MATCHA" diff --quiet "$LAST_SYNC..HEAD" -- "$f"; then
    echo "UNCHANGED  $f"
    continue
  fi
  git -C "$MATCHA" show "$LAST_SYNC:$f" >"$tmp/base" 2>/dev/null || : >"$tmp/base"
  git -C "$MATCHA" show "HEAD:$f" >"$tmp/theirs"
  if [[ -f "$f" ]] && cmp -s "$tmp/base" "$f"; then
    state=TAKE
  else
    state=MERGE
  fi
  echo "$state $(printf '%*s' $((10 - ${#state})) '')$f ($(git -C "$MATCHA" diff --shortstat "$LAST_SYNC..HEAD" -- "$f" | sed 's/^ //'))"
  if ((MERGE)); then
    [[ -f "$f" ]] || cp "$tmp/base" "$f"
    git merge-file -L ours -L "matcha@$LAST_SYNC" -L "matcha@$head" "$f" "$tmp/base" "$tmp/theirs" ||
      echo "           ^ CONFLICT"
  fi
done

echo
echo "== Local includes of matcha's HEAD files that this tree lacks"
# Basenames of every file here (tracked or not) and in matcha, written out once: a `grep -q` on a
# live `git ls-files` pipe fails it under pipefail.
find . -path ./.pio -prune -o -path ./build -prune -o -type f -name '*.h' -printf '%f\n' | sort -u >"$tmp/ours"
git -C "$MATCHA" ls-files '*.h' | sed 's|.*/||' | sort -u >"$tmp/theirs_all"
for f in "${FILES[@]}"; do
  git -C "$MATCHA" show "HEAD:$f" 2>/dev/null | sed -n 's/^#include ["<]\([^">]*\)[">].*/\1/p'
done | sort -u | while read -r inc; do
  case "$inc" in */*) leaf=${inc##*/} ;; *) leaf=$inc ;; esac
  [[ "$leaf" == *.h ]] || continue
  grep -qxF "$leaf" "$tmp/ours" && continue
  grep -qxF "$leaf" "$tmp/theirs_all" && echo "NEW?       $inc"
done

echo
echo "== Reader-glue functions changed in matcha's EpubReaderActivity.cpp"
git -C "$MATCHA" show "$LAST_SYNC:src/activities/reader/EpubReaderActivity.cpp" >"$tmp/era_base"
git -C "$MATCHA" show "HEAD:src/activities/reader/EpubReaderActivity.cpp" >"$tmp/era_head"
python3 -I - "$tmp/era_base" "$tmp/era_head" "${GLUE[@]}" <<'EOF'
import difflib, re, sys

def body(path, name):
    s = open(path, encoding="utf-8").read()
    m = re.search(r"\n[^\n;]*EpubReaderActivity::" + name + r"\([^)]*\)[^;{]*\{", s)
    if not m:
        return None
    i, depth = m.end(), 1
    while depth and i < len(s):
        depth += {"{": 1, "}": -1}.get(s[i], 0)
        i += 1
    return s[m.start():i].splitlines()

for name in sys.argv[3:]:
    a, b = body(sys.argv[1], name), body(sys.argv[2], name)
    if a is None or b is None:
        print(f"{name}: {'added' if b else 'removed' if a else 'not found'} in matcha")
        continue
    diff = [l for l in difflib.unified_diff(a, b, lineterm="", n=0) if l[:1] in "+-" and l[:3] not in ("+++", "---")]
    if diff:
        print(f"{name}: {len(diff)} changed lines")
        for line in diff:
            print("    " + line)
EOF
