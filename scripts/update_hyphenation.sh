#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

process() {
  local lang="$1"

  mkdir -p "build"
  wget -O "build/$lang.bin" "https://github.com/typst/hypher/raw/refs/heads/main/tries/$lang.bin"

  python scripts/generate_hyphenation_trie.py \
    --input "build/$lang.bin" \
    --output "lib/Epub/Epub/hyphenation/generated/hyph-${lang}.trie.h"
}

# German, Russian, Swedish, Ukrainian and Polish are deliberately omitted to
# reclaim flash: together their tries cost ~300 KB, the bulk of all hyphenation
# data. A book in a dropped language still renders, just with ragged margins.
# To restore one, add its `process <tag>` line here and re-add the include,
# LanguageHyphenator and entries() row in LanguageRegistry.cpp (and bump the
# EntryArray size).
process en
process fr
process es
process it
process fi
