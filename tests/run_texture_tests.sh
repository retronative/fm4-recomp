#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The texture cache's CPU side (tests/runtime/test_texture_info.cpp): fetch-constant decode and
# description, untile and endian rules, the write-watch cache model. No Vulkan header, no GPU
# device. Built with warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_texture_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/texture"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_texture_info.cpp" "$ROOT/runtime/video/texture_info.cpp" \
  "$ROOT/runtime/video/texture_untile.cpp" -o "$OUT/test_texture_info"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_texture_info" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
