#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# runtime/kernel/extended_view.h (Extended View: the window's factor, the stretched placement, the HUD narrowing and rule, the latch) and
# runtime/kernel/menu_preview.h (the GRAPHICS submenu's Y: Preview: the blur flattened, menus clipped to the help bar's corner), CPU only,
# ASan/UBSan, warnings as errors; a passing run removes its work directory.
#
#   tests/run_extended_view_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/extended_view"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/kernel" "$ROOT/tests/runtime/test_extended_view.cpp" -o "$OUT/test_extended_view"
status=0
perl -e 'alarm 30; exec @ARGV' "$OUT/test_extended_view" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
