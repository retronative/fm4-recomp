#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp alpha: turns YOUR OWN Forza Motorsport 4 (USA) Disc 1 image into the data this build runs from.
#   ./setup.sh "<path to Forza Motorsport 4 (USA) (Disc 1) (Play Disc).iso>"
# The data goes to $FM4_RECOMP_HOME/data (default: next to this script; FM4 Recomp.app sets ~/Library/Application Support/FM4 Recomp).
# The ISO is only read. Every file the build depends on is checked against the SHA-256 this build was made for: another disc
# version or a damaged extraction is refused, nothing is guessed. Nothing leaves your Mac.
# Prints "progress <percent> <what>" lines (FM4 Recomp.app's progress bar) and "setup: ..." lines.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${FM4_RECOMP_HOME:-$HERE}"
[ $# = 1 ] || { echo "usage: ./setup.sh <Disc 1 .iso>" >&2; exit 2; }
ISO="$1"
[ -f "$ISO" ] || { echo "setup: $ISO is not a file" >&2; exit 1; }
DATA="$ROOT/data"
EXTRACTED_BYTES=8091799701  # the files of FM4 USA Disc 1 (disc1_gdfx.tsv sizes)
rm -f "$DATA/setup.ok"
mkdir -p "$DATA/survey" "$DATA/modules/xmediafacade/survey" "$DATA/modules/speechfacade/survey" "$DATA/user-data" "$DATA/home"
echo "progress 1 Checking the disc image"
if [ -e "$DATA/files" ]; then
  echo "setup: the disc was extracted by an earlier setup: checking it again"
else
  free_kb=$(df -Pk "$DATA" | awk 'NR == 2 {print $4}')
  [ "$free_kb" -gt $((EXTRACTED_BYTES / 1024 + 1024 * 1024)) ] ||
    { echo "setup: not enough free space on the disk of $DATA: $((free_kb / 1024 / 1024)) GB free, 9 GB needed" >&2; exit 1; }
  # A partial extraction from an interrupted setup is this script's own output: removed before starting again.
  PARTIAL="$DATA/files.partial"
  [ ! -e "$PARTIAL" ] || rm -rf "$PARTIAL"
  echo "setup: extracting the disc (a few minutes)..."
  "$HERE/bin/extract-xiso" -x -d "$PARTIAL" "$ISO" > "$DATA/extract.log" 2>&1 & extractor=$!
  while kill -0 "$extractor" 2>/dev/null; do
    done_kb=$(du -sk "$PARTIAL" 2>/dev/null | cut -f1 || echo 0)
    echo "progress $((2 + done_kb * 1024 * 85 / EXTRACTED_BYTES)) Copying the game files from your disc"
    sleep 2
  done
  wait "$extractor" || { echo "setup: extract-xiso failed (see $DATA/extract.log)" >&2; exit 1; }
  mv "$PARTIAL" "$DATA/files"
fi

# The files this build was translated from (FM4 USA Disc 1, title 4D530910, media 26FF1101).
check() {  # <file> <sha-256>
  local got; got="$(shasum -a 256 "$1" | cut -d' ' -f1)"
  [ "$got" = "$2" ] || { echo "setup: $1 is not the file this build needs (SHA-256 $got, expected $2): another disc version?" >&2; exit 1; }
}
echo "progress 88 Checking the game executables"
check "$DATA/files/default.xex" 3618e85af83e972ddcc1b0d0dd808e78e29ab4ab3890a15b31c210a6a3e93e09
check "$DATA/files/XMediaFacade_default.xex" 560bb53fc9a335399e82ba92121410e6781841cbabe3e9dfbf0de43dde7258e9
check "$DATA/files/SpeechFacade_default.xex" b8a5e5137bb18ec42c7e0e2fe5b3e5e760e11e0b0d5ddf0098186995aa7c0f5e

echo "progress 91 Reading the disc's directory"
echo "setup: reading the disc's directory and the executable images..."
"$HERE/bin/gdfx" "$ISO" "$DATA/survey/disc1_gdfx.tsv" > "$DATA/gdfx.log" 2>&1 || { echo "setup: the disc directory could not be read (see $DATA/gdfx.log)" >&2; exit 1; }
echo "progress 94 Loading the game executables"
"$HERE/bin/xexsurvey" "$DATA/files/default.xex" "$DATA/survey" > "$DATA/survey.log" 2>&1
"$HERE/bin/xexsurvey" "$DATA/files/XMediaFacade_default.xex" "$DATA/modules/xmediafacade/survey" >> "$DATA/survey.log" 2>&1
"$HERE/bin/xexsurvey" "$DATA/files/SpeechFacade_default.xex" "$DATA/modules/speechfacade/survey" >> "$DATA/survey.log" 2>&1
echo "progress 98 Checking the loaded images"
check "$DATA/survey/disc1_gdfx.tsv" 92462a7d11062b4ce1878d69a34de2bc485027f36a88dd829dac5d33e5960b33
check "$DATA/survey/image.bin" 776aa2503a6b0a86b449f2f428372882f0b380f532551b166ef2a103f8096d86
check "$DATA/modules/xmediafacade/survey/image.bin" 1db8228818573ca43bfa6e0242f803d1ea7ac0a43e0e1a833169dbc24a04e6c2
check "$DATA/modules/speechfacade/survey/image.bin" cdc0014cb84baaae52b5652280dbb50cbde7d7697df9c475905fbc450d3a9e57
printf 'set up %s from %s\n' "$(date '+%Y-%m-%d %H:%M')" "$(basename "$ISO")" > "$DATA/setup.ok"  # FM4 Recomp.app: setup is complete
echo "progress 100 Ready"
echo "setup: done. Play with ./play.sh"
