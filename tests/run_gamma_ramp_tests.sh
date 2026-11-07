#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The display gamma ramp model, CPU only (tests/runtime/test_gamma_ramp.cpp with runtime/kernel/gamma_ramp.cpp): write
# sequences -> table entries, the PWL application. No guest memory, no Vulkan. Built with warnings as errors under
# ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_gamma_ramp_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/gamma_ramp"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/kernel" "$ROOT/runtime/kernel/gamma_ramp.cpp" "$ROOT/tests/runtime/test_gamma_ramp.cpp" -o "$OUT/test_gamma_ramp"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_gamma_ramp" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
