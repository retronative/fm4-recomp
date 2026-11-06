#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Runtime T0 tests (tests/runtime/test_runtime.cpp) against a synthetic guest layout
# (tests/runtime/ppc_config.h), so they need neither ppc/ nor the game: guard naming, code-address
# check, CRC-32, big-endian slot writes, page protection, the checked indirect call, every stop's exit
# code and the stop lines. Built with warnings as errors.
#
#   tests/run_runtime_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
XR="$ROOT/tools/XenonRecomp"
OUT="$ROOT/build/tests/runtime"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -Wall -Wextra -Werror -isysroot "$SDKROOT" \
  -I"$ROOT/tests/runtime" -I"$ROOT/runtime" -I"$ROOT/recomp" -I"$XR/XenonUtils" -I"$XR/thirdparty/simde" \
  -I"$ROOT/runtime/kernel" "$ROOT/tests/runtime/test_runtime.cpp" "$ROOT/runtime/stops.cpp" "$ROOT/runtime/guest_memory.cpp" \
  "$ROOT"/runtime/kernel/*.cpp -o "$OUT/test_runtime"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_runtime" || status=$?
# A passing run leaves nothing behind; a failing one keeps its binary (KEEP_WORK=1 keeps it either way).
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
