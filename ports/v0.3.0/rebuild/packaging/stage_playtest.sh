#!/bin/bash
# Package the ShiftGlue-only QHD playtest with the existing disc installer and GPU safeguards.
set -euo pipefail
[ "$#" = 6 ] || { echo "usage: $0 <fm4-recomp> <base.app> <engine package> <reference host> <reference sdk> <new output folder>" >&2; exit 2; }
project=$1
base=$2
package=$3
host=$4
sdk=$5
dest=$6
kit_patch="$(cd "$(dirname "$0")" && pwd)/shiftglue-kit.patch"
[ -d "$base/Contents/Resources/kit" ] && [ -x "$package/fm4_static_recomp" ]
mkdir "$dest"
app="$dest/FM4 Recomp.app"
kit="$app/Contents/Resources/kit"
original="$base/Contents/Resources/kit"
engine="$kit/engines/shiftglue"
mkdir -p "$app/Contents/MacOS" "$kit"/{bin,lib} "$engine/licenses"
cp -c "$base/Contents/Info.plist" "$app/Contents/"
cp -Rc "$base/Contents/Resources/"{FM4Recomp.icns,licenses} "$app/Contents/Resources/"
cp -Rc "$original/"{locations.sh,setup.sh,resources,logo.svg,licenses,vulkan} "$kit/"
cp "$(dirname "$kit_patch")/playtest-readme.md" "$kit/README.md"
cp -c "$original/bin/"{extract-xiso,fm4_validation_control,gdfx,xexsurvey} "$kit/bin/"
# The last windowed run used the kit's MoltenVK driver with the engine's own loader.
# The debug control uses the kit's loader and validation layer. xexsurvey uses LZX.
cp -c "$original/lib/"{libfm4_lzx.dylib,libMoltenVK.dylib,libvulkan.1.dylib,libVkLayer_khronos_validation.dylib} "$kit/lib/"
for item in fm4_static_recomp librexgpu-fh1.dylib librexruntime.dylib libfm4_static_recomp_XMediaFacade_default.dylib libfm4_static_recomp_SpeechFacade_default.dylib graphics-settings.json upscalers vulkan; do
  cp -Rc "$package/$item" "$engine/$item"
done
cp "$host/LICENSE" "$engine/licenses/reference-host.txt"
cp "$host/THIRD_PARTY_NOTICES.md" "$engine/licenses/reference-host-notices.md"
cp "$sdk/LICENSE" "$engine/licenses/shiftglue-sdk.txt"
build=$(mktemp -d "${TMPDIR:-/tmp}/fm4-shiftglue-kit.XXXXXXXX")
trap 'rm -rf "$build"' EXIT
mkdir -p "$build/app/setup" "$build/scripts/alpha"
cp "$project"/app/setup/*.swift "$build/app/setup/"
cp "$project/scripts/alpha/play.sh" "$build/scripts/alpha/"
if git -C "$build" apply --check "$kit_patch" 2>/dev/null; then
  git -C "$build" apply "$kit_patch"
else
  # The distributable launcher source may already contain this complete overlay.
  git -C "$build" apply --reverse --check "$kit_patch"
fi
install -m 755 "$build/scripts/alpha/play.sh" "$kit/play.sh"
cp "$project/scripts/alpha/shiftglue-playtest.sh" "$build/scripts/alpha/"
settings_patch="$(dirname "$kit_patch")/graphics-defaults.patch"
if git -C "$build" apply --check "$settings_patch" 2>/dev/null; then
  git -C "$build" apply "$settings_patch"
else
  git -C "$build" apply --reverse --check "$settings_patch"
fi
install -m 755 "$build/scripts/alpha/shiftglue-playtest.sh" "$kit/shiftglue-playtest.sh"
/usr/bin/xcrun swiftc -O -swift-version 5 -target arm64-apple-macos26.0 -parse-as-library -module-name FM4RecompSetup \
  -module-cache-path "$build/ModuleCache" -file-prefix-map "$build=fm4-recomp" \
  "$build"/app/setup/*.swift -o "$app/Contents/MacOS/FM4Recomp"
/usr/libexec/PlistBuddy -c 'Set :CFBundleIdentifier local.fm4recomp.shiftglue-playtest' "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c 'Set :CFBundleShortVersionString 0.2.0-performance-playtest' "$app/Contents/Info.plist"
codesign --force -s - "$app"
codesign --verify --deep --strict "$app"
"$app/Contents/MacOS/FM4Recomp" --setup prerequisites
cat > "$dest/Play QHD Performance.command" <<'LAUNCH'
#!/bin/bash
set -euo pipefail
umask 077
dir=$(cd "$(dirname "$0")" && pwd)
root="$HOME/Library/Application Support/FM4 Recomp ShiftGlue Playtest"
mkdir -p "$root"
if [ ! -e "$root/locations.conf" ]; then
  source "$dir/FM4 Recomp.app/Contents/Resources/kit/locations.sh"
  for original in "$HOME/Library/Application Support/FM4 Recomp Playtest" "$HOME/Library/Application Support/FM4 Recomp"; do
    [ -d "$original" ] || continue
    loc_resolve "$original" || continue
    [ -f "$LOC_DATA_ROOT/setup.ok" ] && [ -f "$LOC_DATA_ROOT/files/default.xex" ] || continue
    loc_set "$root" data_root "$LOC_DATA_ROOT"
    break
  done
fi
if [ "${1:-}" = --check ]; then
  exec env FM4_RECOMP_HOME="$root" "$dir/FM4 Recomp.app/Contents/MacOS/FM4Recomp" --setup launch-check
fi
exec open -n --env FM4_RECOMP_HOME="$root" "$dir/FM4 Recomp.app"
LAUNCH
chmod +x "$dest/Play QHD Performance.command"
