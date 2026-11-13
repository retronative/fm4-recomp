#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp alpha: turns YOUR OWN Forza Motorsport 4 (USA) disc images into the data this build runs from.
#   ./setup.sh [--data <dir>] [--user-data <dir>] "<Forza Motorsport 4 (USA) (Disc 1) (Play Disc).iso>"
#       --data       where the game data goes (about 8 GB; default $FM4_RECOMP_HOME/data, or where an earlier setup put it)
#       --user-data  saves and the Disc 2 content the game installs (about 3 GB with Disc 2; default <home>/data/user-data)
#   ./setup.sh --disc2 [--dest <dir>] "<Forza Motorsport 4 (USA) (Disc 2) (Content Install Disc).iso>"   optional, after Disc 1
#       --dest       the folder for Disc 2's files (about 3.2 GB; default <data>/disc2); it must not exist, be empty, or hold an
#                    earlier Disc 2 extraction. The game installs Disc 2's content packages itself, into the user-data folder
#                    (about 3.0 GB more there). Also run by the game when the player chooses "Install Disc 2" before setup.
# Any folder may be on an external drive. The chosen folders are recorded in $FM4_RECOMP_HOME/locations.conf (format:
# locations.sh) for play.sh and the game, only after the step has passed its checks.
# $FM4_RECOMP_HOME defaults to this script's folder (FM4 Recomp.app sets ~/Library/Application Support/FM4 Recomp).
# The ISOs are only read. Every file the build depends on is checked against the SHA-256 this build was made for: another disc
# version or a damaged extraction is refused, nothing is guessed. Free space is checked on every destination volume first.
# Nothing leaves your Mac.
# Output (stdout, for FM4 Recomp.app and the game): "progress <percent> <what>" lines, then one "result <name>" line:
#   ok 0 | failed 1 | usage 2 | wrong-disc 3 | no-space 4 | extract-failed 5 | cancelled 6 (SIGTERM or SIGINT) | not-set-up 7
#   (Disc 2 before Disc 1) | unsupported 8 (this build has no checksums for Disc 2 yet); the exit status is the number.
# "setup: ..." lines on stderr explain each step and failure.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${FM4_RECOMP_HOME:-$HERE}"
source "$HERE/locations.sh"
RESULT=""
EXTRACTOR=""
finish() {  # <exit status> <result name> [<message>]
  [ $# -lt 3 ] || echo "setup: $3" >&2
  RESULT="$2"
  echo "result $2"
  exit "$1"
}
cancel() {
  [ -z "$EXTRACTOR" ] || { kill "$EXTRACTOR" 2>/dev/null || true; wait "$EXTRACTOR" 2>/dev/null || true; }
  finish 6 cancelled "cancelled; the files copied so far are kept and removed when setup runs again"
}
trap cancel TERM INT
trap '[ -n "$RESULT" ] || echo "result failed"' EXIT
usage() { finish 2 usage "usage: ./setup.sh [--data <dir>] [--user-data <dir>] <Disc 1 .iso> | ./setup.sh --disc2 [--dest <dir>] <Disc 2 .iso>"; }

DISC=1; DATA_ARG=""; USER_ARG=""; DEST_ARG=""
[ "${1:-}" != --disc2 ] || { DISC=2; shift; }
while [ $# -gt 1 ]; do
  case "$1:$DISC" in
    --data:1) DATA_ARG="$2" ;;
    --user-data:1) USER_ARG="$2" ;;
    --dest:2) DEST_ARG="$2" ;;
    *) usage ;;
  esac
  shift 2
done
[ $# = 1 ] || usage
ISO="$1"
case "$ISO" in -*) usage ;; esac
[ -f "$ISO" ] || finish 3 wrong-disc "$ISO is not a file"
loc_resolve "$ROOT" || finish 1 failed "$(loc_file "$ROOT") cannot be read"
check() {  # <file> <sha-256>
  local got; got="$(shasum -a 256 "$1" | cut -d' ' -f1)"
  [ "$got" = "$2" ] || finish 3 wrong-disc "$1 is not the file this build needs (SHA-256 $got, expected $2): another disc version?"
}
# Copies the image's files into <dir> through <dir>.partial (this script's own output, removed when setup starts again).
extract() {  # <dir> <expected bytes> <first percent> <last percent> <log> <what>
  local partial="$1.partial" done_kb status=0
  [ ! -e "$partial" ] || rm -rf "$partial"
  mkdir -p "$(dirname "$1")"
  "$HERE/bin/extract-xiso" -x -d "$partial" "$ISO" > "$5" 2>&1 & EXTRACTOR=$!
  while kill -0 "$EXTRACTOR" 2>/dev/null; do
    done_kb=$(du -sk "$partial" 2>/dev/null | cut -f1 || echo 0)
    echo "progress $(($3 + done_kb * 1024 * ($4 - $3) / $2)) $6"
    sleep 2
  done
  wait "$EXTRACTOR" || status=$?
  EXTRACTOR=""
  [ "$status" = 0 ] || finish 5 extract-failed "extract-xiso failed (see $5)"
  mv "$partial" "$1"
}

if [ "$DISC" = 2 ]; then
  # FM4 USA Disc 2 (Content Install Disc) as extract-xiso writes it: 95 files, 3169575934 bytes, of which the four content packages
  # (installed by the game into the user-data folder) are 3004952576 bytes.
  DISC2_BYTES=3169575934
  PACKAGE_BYTES=3004952576
  PKGDIR="content/0000000000000000/4d530910/00000002"
  PACKAGES=(4d53091000000001 4d53091000000002 4d53091000000003 4d53091000000004)
  PACKAGE_SIZES=(730218496 840794112 579788800 854151168)
  PACKAGE_SHA256=(890a09685065fb010b9d1988f661b3e3a463cad40346482c36077447d0fc5859 7e3508c891a3dce3b0789bf87eadd574bae60c88d9e0c1b6632b2404a99b27c2
                  12348d67b19dca148192840cdea6258c3eba9d3350ec92b55f380c1130e80e01 c8378235d78803c3dd9fb4e4f8d6c43bf28ec41595eca4bc3913098fb6f57b22)
  # From the developers' scripts/prepare_disc2.sh run on the same disc. PENDING until then: setup refuses Disc 2 (unsupported).
  DISC2_GDFX_SHA256=0b474c90c0b6f67e9b432e78b68391356615dadcb4e67571d6d35d41536d5574  # scripts/prepare_disc2.sh of the USA Disc 2
  DATA="$LOC_DATA_ROOT"
  [ -d "$DATA" ] || [ -z "$(loc_get "$ROOT" data_root)" ] ||
    finish 1 failed "the game data folder $DATA is missing: is its drive connected?"
  [ -e "$DATA/setup.ok" ] || finish 7 not-set-up "set up Disc 1 first (./setup.sh <Disc 1 .iso>)"
  DEST="$(loc_abs "${DEST_ARG:-${LOC_DISC2_ROOT:-$DATA/disc2}}")" || finish 2 usage "--dest must be a folder path"
  MAP="$DATA/survey/disc2_gdfx.tsv"
  mkdir -p "$DATA/survey"
  echo "progress 1 Checking the disc image"
  # The disc's directory first (seconds): a wrong disc is refused before anything is copied.
  rm -f "$MAP.partial"
  "$HERE/bin/gdfx" "$ISO" "$MAP.partial" > "$DATA/gdfx2.log" 2>&1 ||
    finish 3 wrong-disc "this is not an Xbox 360 disc image (see $DATA/gdfx2.log)"
  for n in 0 1 2 3; do
    awk -F'\t' -v path="$PKGDIR/${PACKAGES[$n]}" -v size="$(printf '%x' "${PACKAGE_SIZES[$n]}")" '$1 == path && $3 == size { found = 1 }
      END { exit !found }' "$MAP.partial" || finish 3 wrong-disc "this disc image is not Forza Motorsport 4 (USA) Disc 2: content package ${PACKAGES[$n]} is missing"
  done
  [ "$DISC2_GDFX_SHA256" != PENDING ] || finish 8 unsupported "this build has no checksums for Disc 2 yet; Disc 1 alone still plays"
  check "$MAP.partial" "$DISC2_GDFX_SHA256"
  if [ -f "$DEST/default.xex" ] && [ -d "$DEST/$PKGDIR" ]; then
    echo "setup: Disc 2 was extracted to $DEST by an earlier setup: checking it again" >&2
    loc_need "$LOC_USER_DATA_ROOT" "$PACKAGE_BYTES" || finish 4 no-space "the game needs this space to install Disc 2's content"
  else
    [ ! -e "$DEST" ] || [ -z "$(ls -A "$DEST")" ] || finish 1 failed "$DEST is not empty: choose an empty or new folder for Disc 2"
    loc_need "$DEST" "$DISC2_BYTES" "$LOC_USER_DATA_ROOT" "$PACKAGE_BYTES" ||
      finish 4 no-space "Disc 2 needs about 3.2 GB for its files and 3.0 GB in the user-data folder for the content the game installs"
    # Empty (checked above): extract() renames its .partial folder into place. A drive's top folder cannot be removed.
    [ ! -d "$DEST" ] || rmdir "$DEST" 2>/dev/null || finish 1 failed "$DEST cannot be used: choose or create a folder inside it"
    echo "setup: extracting Disc 2 to $DEST (a few minutes)..." >&2
    extract "$DEST" "$DISC2_BYTES" 2 80 "$DATA/extract2.log" "Copying the files from your Disc 2"
  fi
  echo "progress 82 Checking the Disc 2 files"
  check "$DEST/default.xex" 313d11190fc1505b251a303f368fcda11c0fa7cc5dbefddbdbf7f2b5ce278ad0
  for n in 0 1 2 3; do
    echo "progress $((84 + n * 4)) Checking content package $((n + 1)) of 4"
    check "$DEST/$PKGDIR/${PACKAGES[$n]}" "${PACKAGE_SHA256[$n]}"
  done
  mv "$MAP.partial" "$MAP"
  loc_set "$ROOT" disc2_root "$DEST" disc2_map "$MAP" || finish 1 failed "could not record the Disc 2 location"
  echo "progress 100 Ready"
  echo "setup: Disc 2 done (the game installs its content when you choose to)." >&2
  finish 0 ok
fi

# Disc 1.
DATA="$(loc_abs "${DATA_ARG:-$LOC_DATA_ROOT}")" || finish 2 usage "--data must be a folder path"
USER_DATA="$(loc_abs "${USER_ARG:-$LOC_USER_DATA_ROOT}")" || finish 2 usage "--user-data must be a folder path"
EXTRACTED_BYTES=8091799701  # the files of FM4 USA Disc 1 (disc1_gdfx.tsv sizes)
echo "progress 1 Checking the disc image"
if [ -e "$DATA/files" ]; then
  loc_need "$USER_DATA" 0 || finish 4 no-space "the user-data folder needs room for saves"
  echo "setup: the disc was extracted to $DATA by an earlier setup: checking it again" >&2
else
  loc_need "$DATA" "$EXTRACTED_BYTES" "$USER_DATA" 0 || finish 4 no-space "the game data needs about 8 GB (9 GB free)"
fi
mkdir -p "$DATA/survey" "$DATA/modules/xmediafacade/survey" "$DATA/modules/speechfacade/survey" "$DATA/home" "$USER_DATA"
rm -f "$DATA/setup.ok"
if [ ! -e "$DATA/files" ]; then
  echo "setup: extracting the disc to $DATA (a few minutes)..." >&2
  extract "$DATA/files" "$EXTRACTED_BYTES" 2 87 "$DATA/extract.log" "Copying the game files from your disc"
fi

# The files this build was translated from (FM4 USA Disc 1, title 4D530910, media 26FF1101).
echo "progress 88 Checking the game executables"
check "$DATA/files/default.xex" 3618e85af83e972ddcc1b0d0dd808e78e29ab4ab3890a15b31c210a6a3e93e09
check "$DATA/files/XMediaFacade_default.xex" 560bb53fc9a335399e82ba92121410e6781841cbabe3e9dfbf0de43dde7258e9
check "$DATA/files/SpeechFacade_default.xex" b8a5e5137bb18ec42c7e0e2fe5b3e5e760e11e0b0d5ddf0098186995aa7c0f5e

echo "progress 91 Reading the disc's directory"
echo "setup: reading the disc's directory and the executable images..." >&2
"$HERE/bin/gdfx" "$ISO" "$DATA/survey/disc1_gdfx.tsv" > "$DATA/gdfx.log" 2>&1 || finish 3 wrong-disc "the disc directory could not be read (see $DATA/gdfx.log)"
echo "progress 94 Loading the game executables"
"$HERE/bin/xexsurvey" "$DATA/files/default.xex" "$DATA/survey" > "$DATA/survey.log" 2>&1
"$HERE/bin/xexsurvey" "$DATA/files/XMediaFacade_default.xex" "$DATA/modules/xmediafacade/survey" >> "$DATA/survey.log" 2>&1
"$HERE/bin/xexsurvey" "$DATA/files/SpeechFacade_default.xex" "$DATA/modules/speechfacade/survey" >> "$DATA/survey.log" 2>&1
echo "progress 98 Checking the loaded images"
check "$DATA/survey/disc1_gdfx.tsv" 92462a7d11062b4ce1878d69a34de2bc485027f36a88dd829dac5d33e5960b33
check "$DATA/survey/image.bin" 776aa2503a6b0a86b449f2f428372882f0b380f532551b166ef2a103f8096d86
check "$DATA/modules/xmediafacade/survey/image.bin" 1db8228818573ca43bfa6e0242f803d1ea7ac0a43e0e1a833169dbc24a04e6c2
check "$DATA/modules/speechfacade/survey/image.bin" cdc0014cb84baaae52b5652280dbb50cbde7d7697df9c475905fbc450d3a9e57
loc_set "$ROOT" data_root "$DATA" user_data_root "$USER_DATA" || finish 1 failed "could not record the data location"
printf 'set up %s from %s\n' "$(date '+%Y-%m-%d %H:%M')" "$(basename "$ISO")" > "$DATA/setup.ok"  # FM4 Recomp.app: setup is complete
echo "progress 100 Ready"
echo "setup: done. Play with ./play.sh" >&2
finish 0 ok
