#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# runtime/video/zpd_report.h (M9, FM4_OCCLUSION_QUERIES: occlusion query reports counted on the GPU, adapted from ReCarbon), CPU only,
# ASan/UBSan, warnings as errors; a passing run removes its work directory.
#
#   tests/run_zpd_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/zpd"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_zpd_report.cpp" -o "$OUT/test_zpd_report"
status=0
perl -e 'alarm 30; exec @ARGV' "$OUT/test_zpd_report" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
