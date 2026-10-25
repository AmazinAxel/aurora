#!/usr/bin/env bash
#
# Regenerates the committed font headers in src/assets/.
#
# Usage: tools/make_fonts.sh [ttf-dir]
#
# Defaults to tools/ttf/ (gitignored): BarlowSemiCondensed-Black.ttf and
# BarlowSemiCondensed-SemiBold.ttf from Google Fonts (SIL OFL 1.1). Only needed
# when a size or weight changes --
# the generated headers are committed, so an ordinary build does not run this.
#
# Adafruit's fontconvert needs freetype and a C compiler, neither of which is
# a normal dependency of this project, so both come from a nix-shell and the
# binary is built into a temporary directory rather than installed.

set -euo pipefail

cd "$(dirname "$0")/.."

TTF_DIR="${1:-tools/ttf}"
ASSETS="src/assets"
GFX=".pio/libdeps/watch/Adafruit GFX Library/fontconvert/fontconvert.c"

if [ ! -f "$GFX" ]; then
  echo "fontconvert.c not found -- run 'pio run' once so PlatformIO fetches" >&2
  echo "the Adafruit GFX library, then try again." >&2
  exit 1
fi

WORK="$(mktemp -d)"

# Patched on the way through to honour FC_XSCALE: a horizontal-only FreeType
# transform, applied after hinting, that stretches glyphs and their advances.
# Built next to the original so its relative "../gfxfont.h" include resolves.
echo "building fontconvert..."
PATCHED="$(dirname "$GFX")/_fontconvert_xscale.c"
trap 'rm -rf "$WORK" "$PATCHED"' EXIT
sed -e '/FT_Set_Char_Size/a\  { const char *xs = getenv("FC_XSCALE"); if (xs) { FT_Matrix m = {(FT_Fixed)(atof(xs) * 0x10000L), 0, 0, 0x10000L}; FT_Set_Transform(face, \&m, NULL); } }' \
    -e '1i\#include <stdlib.h>' "$GFX" > "$PATCHED"
nix-shell -p gcc freetype pkg-config --run \
  "gcc -o '$WORK/fontconvert' '$PATCHED' \$(pkg-config --cflags --libs freetype2) -lm"

# Each face carries only the characters it draws: fontconvert takes a
# contiguous range, then subset_font.mjs strips everything else from it.
gen() {
  local ttf="$1" size="$2" chars="$3" out="$4" xscale="${5:-1}"
  local first last
  first=$(printf '%d' "'$(printf '%s' "$chars" | fold -w1 | LC_ALL=C sort | head -1)")
  last=$(printf '%d' "'$(printf '%s' "$chars" | fold -w1 | LC_ALL=C sort | tail -1)")
  echo "  $out  ($size pt, \"$chars\")"
  FC_XSCALE="$xscale" nix-shell -p freetype --run \
    "'$WORK/fontconvert' '$TTF_DIR/$ttf' $size $first $last" > "$ASSETS/$out"
  node tools/subset_font.mjs "$ASSETS/$out" "$chars"
}

echo "generating..."
gen BarlowSemiCondensed-Black.ttf    70 -0123456789    TimeBig.h
# Digits plus the letters of SU..SA, stretched; see src/fonts.h.
gen BarlowSemiCondensed-SemiBold.ttf 13 0123456789AEFHMORSTUW Strip.h 1.1

echo "done."
