#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the profile compatibility update from the authenticated 0.3.0 installer.
set -euo pipefail
[ "$#" = 2 ] || { echo "usage: $0 <0.3.0.dmg> <new .app path>" >&2; exit 2; }
image=$1
dest=$2
[ ! -e "$dest" ] && [ ! -L "$dest" ] || { echo "Output already exists." >&2; exit 1; }
root=$(cd "$(dirname "$0")/../../.." && pwd)
expected=dafb8a792c72a405f71c7c886bf1246db66f405062121585eb45c791942bc967
actual=$(shasum -a 256 "$image")
[ "${actual%% *}" = "$expected" ] || { echo "Expected the published 0.3.0 disk image." >&2; exit 1; }
mkdir -p "$(dirname "$dest")"
work=$(mktemp -d "$(dirname "$dest")/.profile-update.XXXXXXXX")
mount="$work/mount"
app="$work/FM4 Recomp.app"
kit="$app/Contents/Resources/kit"
cleanup() {
  if [ -d "$mount/FM4 Recomp.app" ]; then hdiutil detach "$mount" -quiet || return 1; fi
  rm -rf "$work"
}
trap cleanup EXIT
mkdir "$mount"
hdiutil attach -readonly -nobrowse -mountpoint "$mount" "$image" -quiet
/usr/bin/ditto "$mount/FM4 Recomp.app" "$app"
hdiutil detach "$mount" -quiet
for name in play.sh setup.sh shiftglue-playtest.sh migrate-career.sh migrate-playtest-profile.sh; do
  install -m 755 "$root/scripts/alpha/$name" "$kit/$name"
done
install -m 644 "$root/scripts/release/profile-update/kit-readme.md" "$kit/README.md"
compiler=(/usr/bin/xcrun swiftc -O -j 1 -swift-version 5 -target arm64-apple-macos26.0 -module-cache-path "$work/ModuleCache" -file-prefix-map "$root=fm4-recomp" -file-prefix-map "$work=fm4-build")
"${compiler[@]}" "$root/scripts/alpha/migrate-career.swift" -o "$kit/bin/fm4_migrate_career"
"${compiler[@]}" -parse-as-library -module-name FM4RecompSetup "$root"/app/setup/*.swift -o "$app/Contents/MacOS/FM4Recomp"
plist="$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier org.retronative.fm4recomp' "$plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleShortVersionString 0.3.1' "$plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleVersion 10' "$plist"
/usr/bin/codesign --force --sign - "$app"
/usr/bin/codesign --verify --deep --strict "$app"
FM4_RECOMP_HOME="$work/qa-home" "$app/Contents/MacOS/FM4Recomp" --setup prerequisites
mv "$app" "$dest"
