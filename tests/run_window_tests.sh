#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The window mode's pure parts, CPU only (tests/runtime/test_window.cpp): the swapchain choices from capability
# structs, the letterbox rectangle, result classification, the (re)creation planner, the frame hand-over (two threads),
# SDL's event mapping and the stop-on-close path. Vulkan and SDL headers only: nothing is linked or called, no window,
# no device. Built with warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_window_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/window"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" -I/opt/homebrew/include "$ROOT/tests/runtime/test_window.cpp" -o "$OUT/test_window"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_window" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
