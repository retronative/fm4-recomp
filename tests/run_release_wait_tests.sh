#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp (M7, the near road after a race start): the wait of a failed resource-heap allocation for queued releases
# (runtime/game/release_wait.h; tests/runtime/test_release_wait.cpp), CPU only. Built with
# warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_release_wait_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/release_wait"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/game" "$ROOT/tests/runtime/test_release_wait.cpp" -o "$OUT/test_release_wait"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_release_wait" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
