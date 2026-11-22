#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# runtime/video/mem_ledger.h (M9, FM4_INTERNAL_RES), CPU only, ASan/UBSan, warnings as errors; a passing run removes its work
# directory.
#
#   tests/run_mem_ledger_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/mem_ledger"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_mem_ledger.cpp" -o "$OUT/test_mem_ledger"
status=0
perl -e 'alarm 30; exec @ARGV' "$OUT/test_mem_ledger" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
