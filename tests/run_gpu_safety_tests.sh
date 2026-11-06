#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# GPU safety T0 tests (tests/runtime/test_gpu_safety.cpp), CPU only: no GPU device is created. The
# submission gate and lifetime, the fence-wait policy, the loop-work guard, the dump ring and the
# GPU_STOP check (runtime/gpu/, header-only). Built with warnings as errors.
#
#   tests/run_gpu_safety_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/gpu_safety"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -Wall -Wextra -Werror -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" "$ROOT/tests/runtime/test_gpu_safety.cpp" -o "$OUT/test_gpu_safety"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_gpu_safety" "$OUT" || status=$?
# A passing run leaves nothing behind; a failing one keeps its binary and files (KEEP_WORK=1 keeps them either way).
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
