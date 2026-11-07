#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The pure part of the Vulkan EDRAM unit (runtime/video/edram_vk_plan.{h,cpp}) on top of the CPU model
# (runtime/video/edram.cpp), CPU only (tests/runtime/test_edram_vk.cpp): the Vulkan headers for enums, no Vulkan
# library, no GPU device. Built with warnings as errors under ASan/UBSan, as tests/run_edram_tests.sh does; a passing
# run removes its work directory.
#
#   tests/run_edram_vk_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/edram_vk"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" -I/opt/homebrew/include \
  "$ROOT/runtime/video/edram.cpp" "$ROOT/runtime/video/edram_vk_plan.cpp" "$ROOT/tests/runtime/test_edram_vk.cpp" -o "$OUT/test_edram_vk"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_edram_vk" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
