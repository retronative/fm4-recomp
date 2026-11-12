#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp (M7, FM4_ASYNC_RETIRE): the completion worker (runtime/video/async_retire.h), the deferred-store partition
# (runtime/kernel/deferred_store_parts.h) and the GPU read guard's completion from the worker (tests/runtime/test_async_retire.cpp),
# CPU only: no Vulkan header, no GPU device; a stepped worker with a fake clock and GPU, and one threaded smoke test. Built with
# warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_async_retire_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/async_retire"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" -I"$ROOT/runtime/kernel" "$ROOT/tests/runtime/test_async_retire.cpp" -o "$OUT/test_async_retire"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_async_retire" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
