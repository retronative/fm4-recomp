#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The texture unit's binding path, CPU side (tests/runtime/test_texture_vk.cpp; runtime/video/texture_upload.h):
# ring placement, staging bytes, sampler and view keys of the observed fetch constants, the validity policy and
# the GPU-write log. No Vulkan header, no GPU device. Built with warnings as errors under ASan/UBSan; a
# passing run removes its work directory.
#
#   tests/run_texture_vk_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/texture_vk"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_texture_vk.cpp" "$ROOT/runtime/video/texture_upload.cpp" \
  "$ROOT/runtime/video/texture_info.cpp" "$ROOT/runtime/video/texture_untile.cpp" -o "$OUT/test_texture_vk"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_texture_vk" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
