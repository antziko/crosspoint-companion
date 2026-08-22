#!/usr/bin/env bash
set -euo pipefail

# Host-side build+run for the PageTokenScan litmus.
# Pure logic test: a stub replaces GfxRenderer.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/page-token-scan"
BINARY="$BUILD_DIR/PageTokenScanTest"

mkdir -p "$BUILD_DIR"

CXX="${CXX:-g++}"

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR/test/page-token-scan/stubs"  # host stubs first
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/src"
  -I"$ROOT_DIR/lib"
  -I"$ROOT_DIR/lib/EpdFont"
  -I"$ROOT_DIR/lib/Utf8"
  -I"$ROOT_DIR/lib/Memory"
)

# Utf8.cpp supplies utf8NextCodepoint, which the CJK half of isSelectable decodes through.
"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT_DIR/test/page-token-scan/PageTokenScanTest.cpp" \
  "$ROOT_DIR/src/util/PageTokenScan.cpp" \
  "$ROOT_DIR/lib/Utf8/Utf8.cpp" \
  -o "$BINARY"

"$BINARY" "$@"
