#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The CPU-side EDRAM render-target model (runtime/video/edram.{h,cpp}), CPU only
# (tests/runtime/test_edram.cpp): no Vulkan header, no GPU device. Built with warnings as errors under
# ASan/UBSan, as tests/run_renderer_guard_tests.sh does; a passing run removes its work directory.
#
#   tests/run_edram_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/edram"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" "$ROOT/runtime/video/edram.cpp" "$ROOT/tests/runtime/test_edram.cpp" -o "$OUT/test_edram"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_edram" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
