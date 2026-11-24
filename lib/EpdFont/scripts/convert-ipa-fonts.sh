#!/bin/bash
# IPA fonts for dictionary pronunciations — Doulos SIL Regular.
#
# One face per built-in reader size so a pronunciation can match the body text. Basic Latin is
# included because a word that needs this font is drawn entirely from it: a pronunciation such
# as [əˈkuː.t̬ɚ] mixes letters, IPA symbols and combining marks that must share one font for the
# marks to sit on their base letters.
#
# --group-max-bytes keeps every compressed group small: the device inflates a whole group into
# one contiguous buffer, and the dictionary screen runs with ~11 KB largest free block on the X3.
#
# Needs fontTools and freetype-py. Run from lib/EpdFont/scripts.

set -e

cd "$(dirname "$0")"

IPA_SOURCE="../builtinFonts/source/DoulosSIL/DoulosSIL-Regular.ttf"
IPA_STRIPPED="$(mktemp --suffix=.ttf)"
trap 'rm -f "$IPA_STRIPPED"' EXIT
IPA_UNICODES="U+0020-007E,U+00E6,U+00F0,U+00F8,U+0127,U+014B,U+0153,U+03B2,U+03B8,U+03C7,U+0250-02AF,U+02B0-02FF,U+0300-036F,U+1D00-1D7F,U+1D80-1DBF,U+1DC0-1DFF,U+20D0-20FF"
IPA_GROUP_MAX_BYTES=4096

python -m fontTools.subset "$IPA_SOURCE" \
  --unicodes="$IPA_UNICODES" \
  --output-file="$IPA_STRIPPED" \
  --no-layout-closure \
  --drop-tables+=GSUB,GPOS,GDEF,Silt

for size in 12 14 16 18; do
  font_name="ipa_${size}_regular"
  python fontconvert.py "$font_name" "$size" "$IPA_STRIPPED" \
    --2bit --compress --group-max-bytes "$IPA_GROUP_MAX_BYTES" \
    --no-default-intervals \
    --additional-intervals 0x0020,0x007E \
    --additional-intervals 0x0250,0x02AF \
    --additional-intervals 0x02B0,0x02FF \
    --additional-intervals 0x0300,0x036F \
    --additional-intervals 0x1D00,0x1D7F \
    --additional-intervals 0x1D80,0x1DBF \
    --additional-intervals 0x1DC0,0x1DFF \
    --additional-intervals 0x20D0,0x20FF \
    --additional-intervals 0x00E6,0x00E6 \
    --additional-intervals 0x00F0,0x00F0 \
    --additional-intervals 0x00F8,0x00F8 \
    --additional-intervals 0x0127,0x0127 \
    --additional-intervals 0x014B,0x014B \
    --additional-intervals 0x0153,0x0153 \
    --additional-intervals 0x03B2,0x03B2 \
    --additional-intervals 0x03B8,0x03B8 \
    --additional-intervals 0x03C7,0x03C7 \
    > "../builtinFonts/${font_name}.h"
  echo "Generated ${font_name}.h"
done
