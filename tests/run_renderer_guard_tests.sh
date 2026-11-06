#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The renderer's integration logic, CPU only
# (tests/runtime/test_gpu_guard.cpp): no Vulkan header, no GPU device. Built with
# warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_renderer_guard_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/renderer_guard"
mkdir -p "$OUT"
rm -rf "$OUT/work" && mkdir "$OUT/work"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_gpu_guard.cpp" -o "$OUT/test_gpu_guard"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_gpu_guard" "$OUT/work" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
