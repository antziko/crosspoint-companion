#!/usr/bin/env bash
# Three-way merge matcha-reader's page translation files into this tree.
#
#   scripts/sync_matcha_translate.sh [path/to/matcha-reader]
#
# base   = each file at the last synced matcha commit (LAST_SYNC below)
# theirs = the file at matcha HEAD
# ours   = the file here
# Our local edits (marked "crosspoint:") survive; conflicts get standard markers.
# After resolving and building, set LAST_SYNC to the printed HEAD.
set -euo pipefail

LAST_SYNC=2576f466
MATCHA=${1:-../matcha-reader}

FILES=(
  src/activities/reader/EpubReaderTranslationActivity.h
  src/activities/reader/EpubReaderTranslationActivity.cpp
  src/activities/reader/PanelTouch.h
  src/components/DictionaryPanel.h
  src/components/DictionaryPanel.cpp
  src/components/PanelTextPages.h
  src/components/PanelTextPages.cpp
  src/components/icons/dictionaryIcons.h
  lib/hal/SdSystemDir.h
  lib/hal/SdSystemDir.cpp
)

head=$(git -C "$MATCHA" rev-parse --short HEAD)
echo "matcha $LAST_SYNC..$head"
git -C "$MATCHA" log --oneline "$LAST_SYNC..HEAD" -- "${FILES[@]}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
conflicts=0
for f in "${FILES[@]}"; do
  if ! git -C "$MATCHA" cat-file -e "HEAD:$f" 2>/dev/null; then
    echo "GONE upstream: $f"
    continue
  fi
  git -C "$MATCHA" show "$LAST_SYNC:$f" >"$tmp/base"
  git -C "$MATCHA" show "HEAD:$f" >"$tmp/theirs"
  if cmp -s "$tmp/base" "$tmp/theirs"; then continue; fi
  if git merge-file -L ours -L "matcha@$LAST_SYNC" -L "matcha@$head" "$f" "$tmp/base" "$tmp/theirs"; then
    echo "merged    $f"
  else
    echo "CONFLICT  $f"
    conflicts=1
  fi
done

# Also review by hand: the reader glue (EpubReaderActivity openTranslationFor), the boot route
# in main.cpp, and new STR_TRANSLATION_* keys in matcha's english.yaml.
git -C "$MATCHA" diff --stat "$LAST_SYNC..HEAD" -- src/activities/reader/EpubReaderActivity.cpp src/main.cpp \
  src/SilentRestart.h lib/I18n/translations/english.yaml
echo "Next: resolve conflicts, pio run, then set LAST_SYNC=$head"
exit $conflicts
