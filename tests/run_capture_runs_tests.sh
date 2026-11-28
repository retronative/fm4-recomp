#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp (FM4_NO_DRAIN): the capture run tracker
# (runtime/video/capture_runs.h; tests/runtime/test_capture_runs.cpp), CPU only. Built with
# warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_capture_runs_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/capture_runs"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_capture_runs.cpp" -o "$OUT/test_capture_runs"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_capture_runs" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
