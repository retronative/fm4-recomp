#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp (render scenario settings): the plan of the game's render scenario override layer (runtime/game/render_scenario.h;
# tests/runtime/test_render_scenario.cpp), CPU only. Built with warnings as errors under ASan/UBSan; a passing run removes its work
# directory.
#
#   tests/run_render_scenario_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/render_scenario"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/game" "$ROOT/tests/runtime/test_render_scenario.cpp" -o "$OUT/test_render_scenario"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_render_scenario" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
