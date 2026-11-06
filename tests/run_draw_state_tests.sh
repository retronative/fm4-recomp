#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The draw state decode, CPU only (tests/runtime/test_draw_state.cpp with runtime/video/draw_state.cpp):
# no Vulkan header, no GPU device, no guest memory. Built with warnings as errors under ASan/UBSan;
# a passing run removes its work directory.
#
#   tests/run_draw_state_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/draw_state"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" "$ROOT/runtime/video/draw_state.cpp" "$ROOT/tests/runtime/test_draw_state.cpp" -o "$OUT/test_draw_state"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_draw_state" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
