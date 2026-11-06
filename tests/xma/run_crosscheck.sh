#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The XMA cross-check (tests/xma/crosscheck.cpp) on one XMA2 file of the disc: our pass + xmaframes adapter against
# an independent decode by FFmpeg's xma2 packet decoder (the Homebrew ffmpeg tool, run as a separate program, never
# linked). PASS: every compared 16-bit sample within 1 after the best alignment, and mostly non-zero (~10 s).
#
#   tests/xma/run_crosscheck.sh <file.xma> <frames> <evidence dir>
set -euo pipefail
[ $# -eq 3 ] || { echo "usage: tests/xma/run_crosscheck.sh <file.xma> <frames> <evidence dir>" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
XMA="$1"; FRAMES="$2"; OUT="$3"
mkdir -p "$OUT"
LLVM=/opt/homebrew/opt/llvm/bin
SDKROOT="$(xcrun --show-sdk-path)"
FF="$ROOT/tools/ffmpeg-xma"
"$LLVM/clang++" -std=c++20 -O1 -Wall -Wextra -Werror -isysroot "$SDKROOT" -I"$ROOT/tests/runtime" -I"$ROOT/runtime" -I"$ROOT/recomp" -I"$ROOT/tools/XenonRecomp/XenonUtils" -I"$ROOT/tools/XenonRecomp/thirdparty/simde" -I"$ROOT/runtime/kernel" -I"$FF/include" \
  "$ROOT/tests/xma/crosscheck.cpp" "$ROOT/runtime/xma_ffmpeg.cpp" "$FF/lib/libavcodec.a" "$FF/lib/libavutil.a" \
  -o "$OUT/crosscheck" 2> "$OUT/build.log" || { cat "$OUT/build.log"; exit 1; }
/opt/homebrew/opt/ffmpeg/bin/ffmpeg -hide_banner -loglevel error -y -c:a xma2 -i "$XMA" -f f32le "$OUT/reference.f32le"
/opt/homebrew/opt/ffmpeg/bin/ffmpeg -hide_banner -version | head -1 > "$OUT/reference_tool.txt"
"$OUT/crosscheck" "$XMA" "$OUT/reference.f32le" "$FRAMES" | tee "$OUT/result.txt"
rm -f "$OUT/reference.f32le" "$OUT/crosscheck"  # decoded game audio stays local and is not kept
