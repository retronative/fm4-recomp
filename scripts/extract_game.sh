#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Unpack the player's own FM4 discs and add-on into a directory outside the repository, and write
# a SHA-256 manifest of every file; or check an existing extraction against its manifest.
#
#   scripts/extract_game.sh <media dir> <out dir>    # refuses if <out dir> has disc1, disc2 or dlc
#   scripts/extract_game.sh --verify <out dir>       # shasum -c of <out dir>/SHA256SUMS
#
# <media dir> holds "... (Disc 1) (Play Disc).iso", "... (Disc 2) (Content Install Disc).iso" and,
# optionally, add-on .zip files (each holds one STFS package; it is unzipped, not unpacked).
# The ISOs are only read. Output: <out>/disc1, <out>/disc2, <out>/dlc, <out>/disc{1,2}.log,
# <out>/SHA256SUMS.
# Disc 2's content packages (PIRS) and the add-on (LIVE) stay packed: no STFS tool is pinned yet.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XISO="$ROOT/tools/extract-xiso/build/extract-xiso"

if [ "${1:-}" = --verify ]; then
  [ $# = 2 ] || { echo "usage: $0 --verify <out dir>" >&2; exit 2; }
  cd "$2"
  shasum -a 256 -c --quiet SHA256SUMS
  echo "extract_game: $(wc -l < SHA256SUMS | tr -d ' ') files match SHA256SUMS"
  exit 0
fi
[ $# = 2 ] || { echo "usage: $0 <media dir> <out dir> | --verify <out dir>" >&2; exit 2; }
MEDIA="$(cd "$1" && pwd)"
OUT="$2"
[ -x "$XISO" ] || { echo "extract_game: $XISO missing: run scripts/setup_tools.sh" >&2; exit 1; }
for d in disc1 disc2 dlc; do
  if [ -e "$OUT/$d" ] && [ -n "$(ls -A "$OUT/$d")" ]; then
    echo "extract_game: $OUT/$d exists and is not empty; refusing to overwrite" >&2; exit 1
  fi
done
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
shopt -s nullglob
for n in 1 2; do
  isos=("$MEDIA"/*"(Disc $n)"*.iso)
  [ ${#isos[@]} = 1 ] || { echo "extract_game: expected one Disc $n ISO in $MEDIA, found ${#isos[@]}" >&2; exit 1; }
  mkdir -p "$OUT/disc$n"
  nice "$XISO" -x -d "$OUT/disc$n" "${isos[0]}" > "$OUT/disc$n.log" 2>&1
  tail -1 "$OUT/disc$n.log"
done
zips=("$MEDIA"/*.zip)
if [ ${#zips[@]} -gt 0 ]; then
  mkdir -p "$OUT/dlc"
  for z in "${zips[@]}"; do unzip -q -n "$z" -d "$OUT/dlc"; done
fi
cd "$OUT"
dirs=(disc1 disc2)
[ -d dlc ] && dirs+=(dlc)
find "${dirs[@]}" -type f -print0 | sort -z | xargs -0 shasum -a 256 > SHA256SUMS
echo "extract_game: $(wc -l < SHA256SUMS | tr -d ' ') files in $OUT/SHA256SUMS"
