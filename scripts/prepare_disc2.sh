#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The runtime's Disc 2 map for a developer run: --disc2 is the extracted Disc 2 root itself (scripts/extract_game.sh, read-only;
# nothing is copied), --disc2-map is <out>/disc2_gdfx.tsv (survey/gdfx.py of the Disc 2 ISO, the format of disc1_gdfx.tsv).
# The game installs Disc 2's four content packages (PIRS, kept packed) into its user data itself.
#   <out>/disc2_gdfx.tsv   <out>/SHA256SUMS (the map's; its value is scripts/alpha/setup.sh's DISC2_GDFX_SHA256)
#
#   scripts/prepare_disc2.sh <Disc 2 .iso> <extracted disc2 dir> <out dir>    (~5 s)
#
# The ISO and the extraction are only read. Refuses if <out> exists. Each content package must be in the map with the size of
# the extracted file (the map and the extraction come from the same disc).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY=/opt/homebrew/bin/python3
[ $# = 3 ] || { echo "usage: $0 <Disc 2 .iso> <extracted disc2 dir> <out dir>" >&2; exit 2; }
ISO="$1"; DISC2="$2"; OUT="$3"
[ -f "$ISO" ] || { echo "prepare_disc2: $ISO is not a file" >&2; exit 1; }
[ -f "$DISC2/default.xex" ] && [ -d "$DISC2/content" ] || { echo "prepare_disc2: $DISC2 is not an extracted Disc 2" >&2; exit 1; }
[ ! -e "$OUT" ] || { echo "prepare_disc2: $OUT exists; refusing to overwrite" >&2; exit 1; }
PKGDIR="content/0000000000000000/4d530910/00000002"
PACKAGES=(4d53091000000001 4d53091000000002 4d53091000000003 4d53091000000004)
found="$(cd "$DISC2/$PKGDIR" && ls -A)"
[ "$found" = "$(printf '%s\n' "${PACKAGES[@]}")" ] ||
  { echo "prepare_disc2: $DISC2/$PKGDIR holds [$(echo $found)], expected ${PACKAGES[*]}" >&2; exit 1; }

mkdir -p "$OUT"
"$PY" -B -I "$ROOT/survey/gdfx.py" "$ISO" "$OUT/disc2_gdfx.tsv"
for p in "${PACKAGES[@]}"; do
  size=$(stat -f %z "$DISC2/$PKGDIR/$p")
  awk -F'\t' -v path="$PKGDIR/$p" -v size="$(printf '%x' "$size")" '$1 == path && $3 == size { found = 1 }
    END { exit !found }' "$OUT/disc2_gdfx.tsv" ||
    { echo "prepare_disc2: $PKGDIR/$p ($size bytes) is not in disc2_gdfx.tsv with that size" >&2; exit 1; }
done
(cd "$OUT" && shasum -a 256 disc2_gdfx.tsv) > "$OUT/SHA256SUMS"
echo "prepare_disc2: $(wc -l < "$OUT/disc2_gdfx.tsv" | tr -d ' ') entries; $(cat "$OUT/SHA256SUMS")"
