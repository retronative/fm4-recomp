#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp (M7, vertex reuse across submissions): the vertex cache's placement and lifetime policy
# (runtime/video/vertex_cache.h; tests/runtime/test_vertex_cache.cpp), CPU only: no Vulkan header, no GPU device. Built with
# warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_vertex_cache_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/vertex_cache"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_vertex_cache.cpp" -o "$OUT/test_vertex_cache"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_vertex_cache" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
