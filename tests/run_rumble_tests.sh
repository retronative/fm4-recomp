#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# runtime/kernel/rumble.h (controller rumble: the SDL_RumbleGamepad calls planned for the game's requests; ReCarbon's approach), CPU only,
# ASan/UBSan, warnings as errors; a passing run removes its work directory.
#
#   tests/run_rumble_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/rumble"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/kernel" "$ROOT/tests/runtime/test_rumble.cpp" -o "$OUT/test_rumble"
status=0
perl -e 'alarm 30; exec @ARGV' "$OUT/test_rumble" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
