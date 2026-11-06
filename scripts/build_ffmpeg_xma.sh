#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FFmpeg for XMA audio: a minimal static libavcodec + libavutil with only the "xmaframes" decoder (and the WMA Pro
# decoder it is built on), from a pinned source release with patches/ffmpeg/*.patch applied. Fail-closed on the
# licence: the build must configure as LGPL (no GPL, no version 3, no nonfree part) and enable no other decoder.
# Design after ReCarbon scripts/build_ffmpeg_xma.sh (GPL-3.0-or-later); FM4 Recomp changes: the
# xmaframes decoder only (Xenia's frame-at-a-time decoder, patches/ffmpeg/0001), static libraries, the licence
# and decoder checks, and build-info.txt.
#
#   scripts/build_ffmpeg_xma.sh <ffmpeg-<version>.tar.xz> <install prefix> <C compiler>
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: scripts/build_ffmpeg_xma.sh <ffmpeg-<version>.tar.xz> <install prefix> <C compiler>" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARBALL="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
PREFIX="$2"
CC_BIN="$3"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
# The work folder is under tools/ (git-ignored), never /tmp; removed on success, kept on failure.
WORK="$ROOT/tools/ffmpeg-src/build.$$"
rm -rf "$WORK"
mkdir -p "$WORK"
tar -xf "$TARBALL" -C "$WORK"
SRC="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -name 'ffmpeg-*' | head -1)"
[ -n "$SRC" ] || { echo "build_ffmpeg_xma.sh: no ffmpeg-* folder in $TARBALL" >&2; exit 1; }
cd "$SRC"
for p in "$ROOT"/patches/ffmpeg/*.patch; do patch -s -p1 < "$p"; done
SDKROOT="$(xcrun --show-sdk-path)"
./configure --cc="$CC_BIN" --sysroot="$SDKROOT" --prefix=/ \
    --disable-everything --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-postproc --disable-avfilter \
    --enable-decoder=xmaframes --enable-static --disable-shared --enable-pic > "$WORK/configure.log" \
  || { tail -5 "$WORK/configure.log"; tail -15 ffbuild/config.log; echo "build_ffmpeg_xma.sh: configure failed ($WORK)"; exit 1; } >&2
# Licence and contents, from what configure decided (not from the options asked for).
license="$(sed -n 's/^License: //p' "$WORK/configure.log")"
for flag in CONFIG_GPL CONFIG_VERSION3 CONFIG_NONFREE; do
  grep -qx "#define $flag 0" config.h || { echo "build_ffmpeg_xma.sh: $flag is not 0 in config.h ($WORK)" >&2; exit 1; }
done
[ "$license" = "LGPL version 2.1 or later" ] || { echo "build_ffmpeg_xma.sh: licence '$license' ($WORK)" >&2; exit 1; }
decoders="$(grep -E '^#define CONFIG_[A-Z0-9_]+_DECODER 1$' config_components.h | sed 's/#define CONFIG_\(.*\)_DECODER 1/\1/' | tr 'A-Z' 'a-z' | sort | tr '\n' ' ')"
[ "$decoders" = "wmapro xmaframes " ] || { echo "build_ffmpeg_xma.sh: decoders '$decoders' ($WORK)" >&2; exit 1; }
make -j"$(sysctl -n hw.ncpu)" > "$WORK/make.log" 2>&1 || { tail -20 "$WORK/make.log"; echo "build_ffmpeg_xma.sh: make failed ($WORK)"; exit 1; } >&2
make install DESTDIR="$WORK/stage" > "$WORK/install.log"
rm -rf "$PREFIX/include" "$PREFIX/lib"
mv "$WORK/stage/include" "$WORK/stage/lib" "$PREFIX/"
cp COPYING.LGPLv2.1 LICENSE.md "$PREFIX/"
{
  echo "source: $(basename "$TARBALL") sha256 $(shasum -a 256 "$TARBALL" | cut -d' ' -f1)"
  for p in "$ROOT"/patches/ffmpeg/*.patch; do echo "patch: $(basename "$p") sha256 $(shasum -a 256 "$p" | cut -d' ' -f1)"; done
  echo "compiler: $("$CC_BIN" --version | head -1)"
  echo "sdk: $SDKROOT"
  echo "license (configure): $license"
  grep -E '^#define CONFIG_(GPL|VERSION3|NONFREE) ' config.h
  echo "decoders: $decoders"
  echo "configuration: $(sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' config.h)"
} > "$PREFIX/build-info.txt"
cd "$ROOT"
rm -rf "$WORK"
echo "FFmpeg-XMA $(basename "$SRC") in $PREFIX: $license; decoders: $decoders"
