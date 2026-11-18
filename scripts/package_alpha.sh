#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Builds the local macOS alpha package: dist/fm4-recomp-macos-<version>/ with the current build of
# fm4recomp, the tools a player needs to turn their own Disc 1 ISO into the game data (extract-xiso, xexsurvey, gdfx.py), the
# negative-control program of the validation layer, setup.sh, play.sh (the GPU safeguards of scripts/gpu_run.sh, window mode),
# the README and the logo. No game file, decrypted image or translation output is copied. LOCAL ONLY: this package
# does not carry the release kit's licence folder or its separate LGPL libraries (scripts/release/bundle_kit.sh); a release is the DMG.
#   scripts/package_alpha.sh <version> <alpha notes .md>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# = 2 ] || { echo "usage: scripts/package_alpha.sh <version> <alpha notes .md>" >&2; exit 2; }
VERSION="$1"; NOTES="$2"
[ -f "$NOTES" ] || { echo "package_alpha: $NOTES is missing" >&2; exit 1; }
OUT="$ROOT/dist/fm4-recomp-macos-$VERSION"
[ ! -e "$OUT" ] || { echo "package_alpha: $OUT exists (never overwritten)" >&2; exit 1; }
BIN="$ROOT/build/native/runtime/fm4recomp"
for f in "$BIN" "$ROOT/build/native/tests/fm4_validation_control" "$ROOT/tools/extract-xiso/build/extract-xiso" "$ROOT/tools/survey/xexsurvey" \
         "$ROOT/survey/gdfx.py" "$ROOT/assets/fm4-recomp-logo.svg" "$ROOT/scripts/alpha/setup.sh" "$ROOT/scripts/alpha/play.sh" \
         "$ROOT/COPYING" "$ROOT/NOTICE" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/LICENSES"; do
  [ -e "$f" ] || { echo "package_alpha: $f is missing (scripts/build.sh, scripts/setup_tools.sh)" >&2; exit 1; }
done
mkdir -p "$OUT/bin" "$OUT/tools"
cp "$BIN" "$ROOT/build/native/tests/fm4_validation_control" "$ROOT/tools/extract-xiso/build/extract-xiso" "$ROOT/tools/survey/xexsurvey" "$OUT/bin/"
cp "$ROOT/survey/gdfx.py" "$OUT/tools/"
cp "$ROOT/scripts/alpha/setup.sh" "$ROOT/scripts/alpha/play.sh" "$OUT/"
cp "$ROOT/assets/fm4-recomp-logo.svg" "$OUT/logo.svg"
cp "$NOTES" "$OUT/README.md"
cp "$ROOT/COPYING" "$ROOT/NOTICE" "$ROOT/THIRD_PARTY_NOTICES.md" "$OUT/"
cp -R "$ROOT/LICENSES" "$OUT/LICENSES"  # the binary contains Xenia-derived code (BSD-3-Clause: its notice ships with it)
chmod +x "$OUT/setup.sh" "$OUT/play.sh" "$OUT/bin/"*
{
  echo "fm4-recomp macOS alpha $VERSION"
  echo "built $(date '+%Y-%m-%d %H:%M') from commit $(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet || echo ' (with uncommitted changes)')"
  (cd "$OUT" && find . -type f ! -name MANIFEST.txt -print0 | sort -z | xargs -0 shasum -a 256)
} > "$OUT/MANIFEST.txt"
# Never a game file in the package (the .gitignore patterns of game data, and the generated images).
if find "$OUT" -type f \( -name '*.xex' -o -name '*.iso' -o -name 'image.bin' -o -name '*.xma' -o -name '*.fxobj' -o -name 'disc1_gdfx.tsv' \) | grep -q .; then
  echo "package_alpha: a game-derived file entered the package" >&2; exit 1
fi
(cd "$ROOT/dist" && tar -czf "fm4-recomp-macos-$VERSION.tar.gz" "fm4-recomp-macos-$VERSION")
shasum -a 256 "$ROOT/dist/fm4-recomp-macos-$VERSION.tar.gz"
echo "package_alpha: $OUT ($(du -sh "$OUT" | cut -f1))"
