#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Keyboard prompts (runtime/kernel/keyboard_prompts.h: the keyboard wording of FM4's prompt strings; input_live.h LastDevice: the
# device used last), CPU only, ASan/UBSan, warnings as errors; a passing run removes its work directory. With a TSV from
# tests/check_prompt_string_keys.py (the EN prompt strings), it also prints how each one reads for the keyboard.
#
#   tests/run_keyboard_prompt_tests.sh [<EN prompts TSV>]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/keyboard_prompts"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/kernel" "$ROOT/tests/runtime/test_keyboard_prompts.cpp" -o "$OUT/test_keyboard_prompts"
status=0
perl -e 'alarm 30; exec @ARGV' "$OUT/test_keyboard_prompts" "$@" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
