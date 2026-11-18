#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Fills a self-contained kit (every runtime and setup dependency bundled, the app loads only its own
# copies; no Homebrew, Xcode or developer tools on the player's Mac): bin/ (fm4recomp, the validation layer's negative control,
# extract-xiso, xexsurvey, gdfx, fm4recomp-setup: the launcher's setup app, the in-game Disc setup panel), lib/ (SDL3, the Vulkan loader, MoltenVK, the Vulkan validation layer, glslang, SPIRV-Tools),
# vulkan/ (the MoltenVK driver and the validation layer manifests, by relative path), setup.sh, play.sh, locations.sh (their
# shared storage-location helpers), README.md, licenses/.
# The LGPL parts are libraries of their own in lib/, as in ReCarbon (never inside a program, so a user can replace them:
# THIRD_PARTY_NOTICES.md): FFmpeg's libavcodec and libavutil, from the prefix the build linked (FM4_FFMPEG_XMA), and
# libmspack's LZX decoder, libfm4_lzx.dylib, from the build. The build must be the release build
# (FM4_RELEASE_FFMPEG=<prefix> scripts/build.sh); scripts/release/check_lgpl_libs.sh checks the result.
# Every bundled binary and library refers to the others by @rpath (bin: @executable_path/../lib, lib: @loader_path), is checked
# to name no library outside the kit and macOS, and to need no macOS newer than the target; then ad hoc signed.
#   scripts/release/bundle_kit.sh <kit dir> [<deps prefix>]
# <deps prefix>: the libraries built for the target (scripts/release/build_runtime_deps.sh: <prefix>/lib); without it, Homebrew's
# (built for the build Mac's macOS: refused unless ALLOW_NEWER_DEPS=1, for local tests only). MoltenVK is always Homebrew's
# (its minimum is macOS 12).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: scripts/release/bundle_kit.sh <kit dir> [<deps prefix>]" >&2; exit 2; }
KIT="$1"; DEPS="${2:-}"
TARGET=26.0  # the oldest macOS the alpha supports
BREW=/opt/homebrew/opt
CACHE="$ROOT/build/native/CMakeCache.txt"
grep -q '^FM4_LGPL_SHARED:BOOL=ON$' "$CACHE" ||
  { echo "bundle_kit: build/native is not the release build (FM4_RELEASE_FFMPEG=<shared FFmpeg prefix> scripts/build.sh)" >&2; exit 1; }
FFMPEG="$(sed -n 's/^FM4_FFMPEG_XMA:PATH=//p' "$CACHE")"
[ ! -e "$KIT/bin" ] || { echo "bundle_kit: $KIT/bin exists (never overwritten)" >&2; exit 1; }
mkdir -p "$KIT/bin" "$KIT/lib" "$KIT/vulkan/icd.d" "$KIT/vulkan/explicit_layer.d" "$KIT/vulkan/implicit_layer.d" "$KIT/licenses/third-party"
touch "$KIT/vulkan/implicit_layer.d/.keep"  # empty on purpose: play.sh points VK_IMPLICIT_LAYER_PATH here

# name -> Homebrew formula (the source when no deps prefix is given; its licence in every case)
LIBS=(libSDL3.0.dylib:sdl3 libvulkan.1.dylib:vulkan-loader libglslang.16.dylib:glslang libglslang-default-resource-limits.16.dylib:glslang
      libSPIRV.16.dylib:glslang libSPIRV-Tools.dylib:spirv-tools libSPIRV-Tools-opt.dylib:spirv-tools libMoltenVK.dylib:molten-vk
      libVkLayer_khronos_validation.dylib:vulkan-validationlayers)
NAMES=()
for entry in "${LIBS[@]}"; do
  name="${entry%%:*}"; formula="${entry##*:}"; NAMES+=("$name")
  if [ -n "$DEPS" ] && [ "$formula" != molten-vk ]; then src="$DEPS/lib/$name"; else src="$BREW/$formula/lib/$name"; fi
  [ -e "$src" ] || { echo "bundle_kit: $src is missing" >&2; exit 1; }
  cp -L "$src" "$KIT/lib/$name"
  chmod u+w "$KIT/lib/$name"
done
for src in "$FFMPEG/lib/libavcodec.61.dylib" "$FFMPEG/lib/libavutil.59.dylib" "$ROOT/build/native/runtime/libfm4_lzx.dylib"; do
  [ -e "$src" ] || { echo "bundle_kit: $src is missing" >&2; exit 1; }
  name="$(basename "$src")"; NAMES+=("$name")
  cp -L "$src" "$KIT/lib/$name"
  chmod u+w "$KIT/lib/$name"
done
cp "$ROOT/build/native/runtime/fm4recomp" "$ROOT/build/native/tests/fm4_validation_control" "$ROOT/tools/extract-xiso/build/extract-xiso" \
   "$ROOT/build/native/runtime/xexsurvey" "$ROOT/tools/survey/gdfx" "$KIT/bin/"
# The launcher's setup app, also the in-game Disc setup panel (fm4recomp --disc-setup, play.sh; FM4 Recomp --disc-panel): the
# same sources as FM4 Recomp.app (scripts/release/make_app.sh), so the kit's panel is the launcher's own UI.
/usr/bin/xcrun swiftc -O -swift-version 5 -target "arm64-apple-macos$TARGET" -parse-as-library -module-name FM4RecompSetup \
  -file-prefix-map "$ROOT=fm4-recomp" "$ROOT"/app/setup/*.swift -o "$KIT/bin/fm4recomp-setup"
chmod u+w "$KIT"/bin/*

is_bundled() { local n; for n in "${NAMES[@]}"; do [ "$n" = "$1" ] && return 0; done; return 1; }
relink() {  # <file> <rpath>
  local f="$1" dep base r
  for r in $(otool -l "$f" | awk '/LC_RPATH/ {getline; getline; print $2}'); do install_name_tool -delete_rpath "$r" "$f" 2>/dev/null; done
  otool -L "$f" | tail -n +2 | awk '{print $1}' | while read -r dep; do
    case "$dep" in /usr/lib/*|/System/*) continue ;; esac
    base="$(basename "$dep")"
    [ "$dep" = "@rpath/$base" ] && is_bundled "$base" && continue
    is_bundled "$base" || { echo "bundle_kit: $f needs $dep, which is not bundled" >&2; exit 1; }
    install_name_tool -change "$dep" "@rpath/$base" "$f" 2>/dev/null
  done
  install_name_tool -add_rpath "$2" "$f" 2>/dev/null
}
for f in "$KIT"/lib/*.dylib; do
  install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null
  relink "$f" @loader_path
done
for f in "$KIT"/bin/*; do relink "$f" @executable_path/../lib; done

# The Vulkan manifests: library paths relative to the manifest (the loader resolves them from its directory).
cat > "$KIT/vulkan/icd.d/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../lib/libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
JSON
LAYER_JSON="$BREW/vulkan-validationlayers/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
[ -z "$DEPS" ] || LAYER_JSON="$DEPS/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
sed 's|"library_path": "[^"]*"|"library_path": "../../lib/libVkLayer_khronos_validation.dylib"|' "$LAYER_JSON" > "$KIT/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
grep -q '"api_version": "1.4.363"' "$KIT/vulkan/explicit_layer.d/VkLayer_khronos_validation.json" ||
  { echo "bundle_kit: the validation layer is not the pinned 1.4.363" >&2; exit 1; }

cp "$ROOT/scripts/alpha/setup.sh" "$ROOT/scripts/alpha/play.sh" "$ROOT/scripts/alpha/locations.sh" "$ROOT/scripts/alpha/README.md" "$KIT/"
chmod +x "$KIT/setup.sh" "$KIT/play.sh"
cp "$ROOT/COPYING" "$ROOT/NOTICE" "$ROOT/THIRD_PARTY_NOTICES.md" "$KIT/licenses/"
cp -R "$ROOT/LICENSES" "$KIT/licenses/LICENSES"
# The bundled libraries' own licence files, from the same place as the libraries: the pinned sources' copies in the deps
# prefix (build_runtime_deps.sh), or, without a deps prefix, the Homebrew version that $BREW links (local tests only).
# MoltenVK is always Homebrew's, as its library is.
for entry in SDL/LICENSE.txt:sdl3 Vulkan-Loader/LICENSE.txt:vulkan-loader glslang/LICENSE.txt:glslang SPIRV-Tools/LICENSE:spirv-tools \
             Vulkan-ValidationLayers/LICENSE.txt:vulkan-validationlayers MoltenVK/LICENSE:molten-vk; do
  rel="${entry%%:*}"; formula="${entry##*:}"
  if [ -n "$DEPS" ] && [ "$formula" != molten-vk ]; then src="$DEPS/share/licenses/$rel"
  else src="$(cd "$BREW/$formula" && pwd -P)/$(basename "$rel")"; fi
  [ -f "$src" ] || { echo "bundle_kit: the licence $src is missing" >&2; exit 1; }
  mkdir -p "$KIT/licenses/third-party/$formula"
  cp "$src" "$KIT/licenses/third-party/$formula/"
done
# FFmpeg (LGPL): what was built and our change to it.
mkdir -p "$KIT/licenses/third-party/ffmpeg"
cp "$FFMPEG/build-info.txt" "$ROOT"/patches/ffmpeg/*.patch "$KIT/licenses/third-party/ffmpeg/"
grep -qx 'libraries: shared' "$FFMPEG/build-info.txt" || { echo "bundle_kit: $FFMPEG is not a shared build" >&2; exit 1; }
for pair in FFmpeg-LGPL-2.1.txt:COPYING.LGPLv2.1 FFmpeg-LICENSE.md:LICENSE.md; do
  cmp -s "$ROOT/LICENSES/${pair%%:*}" "$FFMPEG/${pair##*:}" || { echo "bundle_kit: LICENSES/${pair%%:*} differs from $FFMPEG/${pair##*:}" >&2; exit 1; }
done

# The committed texts must still be the ones of the pinned tools and sources (a pin change must update LICENSES/).
while read -r text source; do
  cmp -s "$ROOT/LICENSES/$text" "$ROOT/$source" || { echo "bundle_kit: LICENSES/$text differs from $source" >&2; exit 1; }
done <<'PAIRS'
libmspack-LGPL-2.1.txt tools/XenonRecomp/thirdparty/libmspack/libmspack/COPYING.LIB
libmspack-AUTHORS.txt tools/XenonRecomp/thirdparty/libmspack/libmspack/AUTHORS
XenonRecomp-MIT.md tools/XenonRecomp/LICENSE.md
SIMDe-MIT.txt tools/XenonRecomp/thirdparty/simde/COPYING
tiny-AES-c-Unlicense.txt tools/XenonRecomp/thirdparty/tiny-AES-c/unlicense.txt
extract-xiso-BSD-4-Clause.txt tools/extract-xiso/LICENSE.TXT
PAIRS
if [ -n "$DEPS" ]; then
  for name in Vulkan-Loader Vulkan-ValidationLayers; do
    diff -r "$DEPS/share/licenses/$name/LICENSES" "$ROOT/LICENSES/$name" > /dev/null ||
      { echo "bundle_kit: LICENSES/$name differs from the pinned $name sources" >&2; exit 1; }
  done
fi
# Every licence text THIRD_PARTY_NOTICES.md names is in the kit.
for path in $(grep -oE '\]\((LICENSES|third-party)/[^)]*\)' "$KIT/licenses/THIRD_PARTY_NOTICES.md" | sed 's/^](//; s/)$//' | sort -u); do
  [ -e "$KIT/licenses/$path" ] || { echo "bundle_kit: THIRD_PARTY_NOTICES.md names $path, which is not in the kit" >&2; exit 1; }
done

# Checks: nothing outside the kit and macOS; nothing newer than the target.
fail=0
for f in "$KIT"/bin/* "$KIT"/lib/*.dylib; do
  outside="$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -Ev '^(/usr/lib/|/System/|@rpath/)' || true)"
  [ -z "$outside" ] || { echo "bundle_kit: $f still needs $outside" >&2; fail=1; }
  minos="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
  if [ "$(printf '%s\n%s\n' "$minos" "$TARGET" | sort -V | tail -1)" != "$TARGET" ]; then
    echo "bundle_kit: $(basename "$f") needs macOS $minos (target $TARGET)" >&2
    [ "${ALLOW_NEWER_DEPS:-0}" = 1 ] || fail=1
  fi
done
[ $fail = 0 ] || exit 1
"$ROOT/scripts/release/check_lgpl_libs.sh" "$KIT"
strip -S "$KIT"/bin/* 2>/dev/null || true
codesign --force -s - "$KIT"/lib/*.dylib "$KIT"/bin/* 2>/dev/null
for f in "$KIT"/lib/*.dylib "$KIT"/bin/*; do codesign --verify "$f"; done
echo "kit: $KIT ($(du -sh "$KIT" | cut -f1)): $(ls "$KIT/bin" | wc -l | tr -d ' ') programs, $(ls "$KIT/lib" | wc -l | tr -d ' ') libraries, target macOS $TARGET"
