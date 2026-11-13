#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp.app: the guided first-run setup and launcher (app/setup, SwiftUI) as the app, the kit in
# Contents/Resources/kit (fm4recomp and the validation layer's negative control from build/native, extract-xiso and xexsurvey
# and gdfx from tools/, the bundled libraries, setup.sh and play.sh with the GPU safeguards, the README), the icon and the licences. No game file,
# decrypted image or translation output: the player's own Disc 1 is set up on their Mac.
# Adapted from reFrozen scripts/release/make_release_app.sh (GPL-3.0-or-later): the kit layout, debug symbols
# stripped (they name build paths), ad hoc signatures inside out. FM4 Recomp changes: FM4's kit, Homebrew runtime libraries
# (checked by the app, not shipped), the art from assets/dmg/render.mjs.
#
#   scripts/release/make_app.sh <version> <out dir> [<deps prefix (scripts/release/build_runtime_deps.sh)>]
# Every runtime and setup dependency is bundled (scripts/release/bundle_kit.sh) and the app targets macOS 26.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ $# -ge 2 ] && [ $# -le 3 ] || { echo "usage: scripts/release/make_app.sh <version> <out dir> [<deps prefix>]" >&2; exit 2; }
VERSION="$1"; OUT="$2"; DEPS="${3:-}"
APP="$OUT/FM4 Recomp.app"
KIT="$APP/Contents/Resources/kit"
ART="$OUT/art"
for f in "$ROOT/build/native/runtime/fm4recomp" "$ROOT/build/native/tests/fm4_validation_control" "$ROOT/tools/extract-xiso/build/extract-xiso" \
         "$ROOT/tools/survey/xexsurvey" "$ROOT/tools/survey/gdfx" "$ROOT/scripts/alpha/setup.sh" "$ROOT/scripts/alpha/play.sh" "$ROOT/scripts/alpha/locations.sh" "$ROOT/scripts/alpha/README.md"; do
  [ -e "$f" ] || { echo "make_app: $f is missing (scripts/build.sh, scripts/setup_tools.sh)" >&2; exit 1; }
done
[ ! -e "$APP" ] || { echo "make_app: $APP exists (never overwritten)" >&2; exit 1; }
mkdir -p "$APP/Contents/MacOS" "$(dirname "$KIT")" "$ART"

/opt/homebrew/bin/node "$ROOT/assets/dmg/render.mjs" --out "$ART" > "$OUT/art.log"

# The kit: programs, bundled libraries, Vulkan manifests, scripts, README, licences.
"$ROOT/scripts/release/bundle_kit.sh" "$KIT" ${DEPS:+"$DEPS"}
mkdir -p "$KIT/resources"
cp "$ART/logo@2x.png" "$KIT/resources/"
cp "$ROOT/assets/fm4-recomp-logo.svg" "$KIT/logo.svg"

# The app: the setup and launcher, its icon, Info.plist.
/usr/bin/xcrun swiftc -O -swift-version 5 -target arm64-apple-macos26.0 -parse-as-library -module-name FM4RecompSetup \
  -file-prefix-map "$ROOT=fm4-recomp" "$ROOT"/app/setup/*.swift -o "$APP/Contents/MacOS/FM4Recomp"
cp "$ART/FM4Recomp.icns" "$APP/Contents/Resources/"
BUILD_ID="$(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet || echo '-dirty')"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>FM4Recomp</string>
  <key>CFBundleIdentifier</key><string>org.retronative.fm4recomp</string>
  <key>CFBundleName</key><string>FM4 Recomp</string>
  <key>CFBundleDisplayName</key><string>FM4 Recomp</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$BUILD_ID</string>
  <key>CFBundleIconFile</key><string>FM4Recomp</string>
  <key>LSMinimumSystemVersion</key><string>26.0</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>LSApplicationCategoryType</key><string>public.app-category.racing-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>FM4 Recomp is free software under the GNU GPL v3 or later. Not affiliated with Microsoft or Turn 10 Studios. Requires your own copy of the game.</string>
</dict>
</plist>
PLIST

# Licences: FM4 Recomp's and every bundled component's (the kit's licenses/), also where macOS apps keep them.
cp -R "$KIT/licenses" "$APP/Contents/Resources/licenses"

# No debug symbols (they name build paths); ad hoc signatures, inside out.
strip -S "$APP/Contents/MacOS/FM4Recomp" 2>/dev/null || true
codesign --force -s - "$APP"
codesign --verify --deep --strict "$APP"

# Never a game file in the app.
if find "$APP" -type f \( -iname '*.xex' -o -iname '*.iso' -o -name 'image.bin' -o -iname '*.xma' -o -iname '*.fxobj' -o -name 'disc[12]_gdfx.tsv' -o -name '4d530910*' \) | grep -q .; then
  echo "make_app: a game-derived file entered the app" >&2; exit 1
fi
for f in "$APP/Contents/MacOS/FM4Recomp" "$KIT"/bin/* "$KIT"/lib/*.dylib; do
  printf '%s minos %s\n' "${f#$APP/}" "$(otool -l "$f" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
done > "$OUT/deployment-targets.txt"
echo "app: $APP ($(du -sh "$APP" | cut -f1)), version $VERSION ($BUILD_ID); deployment targets in $OUT/deployment-targets.txt"
