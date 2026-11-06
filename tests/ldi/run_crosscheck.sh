#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The LDI (LZX) decoder against the CRC-32 of FM4's zip entries (tests/ldi/crosscheck.cpp). PASS: every method-21
# entry of the zips given decodes to its size and CRC.
#
#   tests/ldi/run_crosscheck.sh <evidence dir> <zip> [<zip> ...]
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: tests/ldi/run_crosscheck.sh <evidence dir> <zip> [<zip> ...]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$1"; shift
mkdir -p "$OUT"
LLVM=/opt/homebrew/opt/llvm/bin
SDKROOT="$(xcrun --show-sdk-path)"
M="$ROOT/tools/XenonRecomp/thirdparty/libmspack/libmspack/mspack"
"$LLVM/clang" -std=c99 -O2 -isysroot "$SDKROOT" -I"$M" -c "$M/lzxd.c" -o "$OUT/lzxd.o"
"$LLVM/clang++" -std=c++20 -O2 -Wall -Wextra -Werror -isysroot "$SDKROOT" -I"$ROOT/runtime" -I"$M" \
  "$ROOT/tests/ldi/crosscheck.cpp" "$ROOT/runtime/ldi_mspack.cpp" "$OUT/lzxd.o" -o "$OUT/crosscheck"
"$OUT/crosscheck" "$@" | tee "$OUT/result.txt"
rm -f "$OUT/crosscheck" "$OUT/lzxd.o"
