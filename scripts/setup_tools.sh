#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Build the translation toolchain into tools/ (git-ignored). Idempotent: re-running reuses what is
# current. Adapted from ForzaRecomp's and ReCarbon's setup_tools.sh (same pins).
#
#   scripts/setup_tools.sh
#
#   tools/XenonRecomp     pinned upstream XenonRecomp plus patches/xenonrecomp/*.patch (git am, in
#                         order) on branch fm4, built: XenonRecomp and XenonAnalyse
#   tools/extract-xiso    pinned extract-xiso, to unpack the player's disc images
#   tools/survey/         one binary per survey/*.cpp, linked against XenonRecomp's libraries
#   tools/ffmpeg-xma      FFmpeg (pinned release, SHA-256 checked) with patches/ffmpeg/*.patch: static libavcodec and
#                         libavutil with only the xmaframes decoder, LGPL (scripts/build_ffmpeg_xma.sh checks it)
#
# Needs git, CMake, Ninja and clang 18 or newer. LLVM=<bin dir> selects the compiler; by default
# Homebrew LLVM. After changing a patch: git -C tools/XenonRecomp checkout -q --detach &&
# git -C tools/XenonRecomp branch -D fm4, then re-run (patches apply only when the branch is missing).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools"
XENONRECOMP_URL=https://github.com/hedge-dev/XenonRecomp.git
XENONRECOMP_COMMIT=ddd128bcca99fe8bfbb99bea583c972351fa6ace
EXTRACT_XISO_URL=https://github.com/XboxDev/extract-xiso.git
EXTRACT_XISO_COMMIT=3f5b62cfe68f000b0e3c8a30104973f3a297948e
BRANCH=fm4
# FFmpeg 7.1.1 release tarball; the SHA-256 agrees with ReCarbon's independent pin (scripts/setup_tools.sh).
FFMPEG_VERSION=7.1.1
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
  else LLVM="$(dirname "$(command -v clang)")"; fi
fi
"$LLVM/clang" --version | head -1
mkdir -p "$TOOLS"
shopt -s nullglob

XR="$TOOLS/XenonRecomp"
[ -d "$XR" ] || git clone -q "$XENONRECOMP_URL" "$XR"
if ! git -C "$XR" rev-parse -q --verify "$BRANCH" >/dev/null; then
  git -C "$XR" checkout -q -b "$BRANCH" "$XENONRECOMP_COMMIT"
  git -C "$XR" submodule update -q --init --recursive
  patches=("$ROOT"/patches/xenonrecomp/*.patch)
  if [ ${#patches[@]} -gt 0 ]; then
    # Patches carry their own authors; the setup only needs some committer identity.
    git -C "$XR" -c user.name="FM4 Recomp setup" -c user.email=setup@fm4-recomp.invalid am -q "${patches[@]}"
  fi
fi
# -include cstdlib: the bundled fmt relies on a transitive <cstdlib> that newer libc++ dropped.
cmake -Wno-deprecated -S "$XR" -B "$XR/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" > /dev/null
ninja -C "$XR/build" XenonRecomp XenonAnalyse

XISO="$TOOLS/extract-xiso"
[ -d "$XISO" ] || git clone -q "$EXTRACT_XISO_URL" "$XISO"
if [ "$(git -C "$XISO" rev-parse HEAD)" != "$EXTRACT_XISO_COMMIT" ]; then
  git -C "$XISO" checkout -q "$EXTRACT_XISO_COMMIT"
  rm -rf "$XISO/build"
fi
if [ ! -x "$XISO/build/extract-xiso" ]; then
  cmake -Wno-deprecated -S "$XISO" -B "$XISO/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
  ninja -C "$XISO/build"
fi

# Survey tools: one binary per survey/*.cpp, rebuilt when the source or XenonRecomp changes.
LIBS=("$XR/build/XenonUtils/libXenonUtils.a" "$XR/build/thirdparty/fmt/libfmt.a"
      "$XR/build/thirdparty/disasm/libdisasm.a" "$XR/build/thirdparty/xxHash/cmake_unofficial/libxxhash.a")
INCLUDES=(-I"$XR/XenonUtils" -I"$XR/thirdparty" -I"$XR/thirdparty/disasm" -I"$XR/thirdparty/fmt/include"
          -I"$XR/thirdparty/xxHash" -I"$XR/thirdparty/TinySHA1" -I"$XR/thirdparty/libmspack/libmspack/mspack"
          -I"$XR/thirdparty/tiny-AES-c")
mkdir -p "$TOOLS/survey"
for src in "$ROOT"/survey/*.cpp; do
  bin="$TOOLS/survey/$(basename "$src" .cpp)"
  stale=0
  [ -x "$bin" ] || stale=1
  for f in "$src" "${LIBS[@]}"; do [ "$f" -nt "$bin" ] && stale=1; done
  if [ $stale = 1 ]; then
    "$LLVM/clang++" -std=c++20 -O2 -include cstdlib "${INCLUDES[@]}" "$src" "${LIBS[@]}" -o "$bin"
    echo "built tools/survey/$(basename "$bin")"
  fi
done
# FFmpeg-XMA: rebuilt when the tarball, a patch or the build script changes (a stamp of their hashes).
FF="$TOOLS/ffmpeg-xma"
mkdir -p "$TOOLS/ffmpeg-src"
tarball="$TOOLS/ffmpeg-src/ffmpeg-$FFMPEG_VERSION.tar.xz"
if [ ! -f "$tarball" ]; then
  curl -sfL -o "$tarball.part" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
  mv "$tarball.part" "$tarball"
fi
echo "$FFMPEG_SHA256  $tarball" | shasum -a 256 -c --quiet || { echo "setup_tools: $tarball does not match its SHA-256" >&2; exit 1; }
stamp="$( { echo "$FFMPEG_SHA256"; shasum -a 256 "$ROOT"/patches/ffmpeg/*.patch "$ROOT/scripts/build_ffmpeg_xma.sh" | cut -d' ' -f1; "$LLVM/clang" --version | head -1; } | shasum -a 256 | cut -d' ' -f1)"
if [ "$(cat "$FF/stamp" 2>/dev/null)" != "$stamp" ]; then
  "$ROOT/scripts/build_ffmpeg_xma.sh" "$tarball" "$FF" "$LLVM/clang"
  echo "$stamp" > "$FF/stamp"
fi

echo "setup_tools: XenonRecomp $(git -C "$XR" rev-parse --short HEAD) on $BRANCH, extract-xiso $(git -C "$XISO" rev-parse --short HEAD), FFmpeg-XMA $FFMPEG_VERSION ($(sed -n 's/^license (configure): //p' "$FF/build-info.txt"))"
