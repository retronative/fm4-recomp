#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# runtime/video/hdr_output.h (M9, FM4_HDR: HDR output, adapted from SpeedBreaker) and window_policy.h's HDR surface format, CPU only,
# ASan/UBSan, warnings as errors: the switches, the expansion curve, the limiters, the format choice, and the emitted compute shader
# through glslangValidator + spirv-val (the runtime compiles the same text with glslang's library for Vulkan 1.0 / SPIR-V 1.0).
# No GPU. A passing run removes its work directory.
#
#   tests/run_hdr_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
export PATH="/opt/homebrew/bin:$PATH"
for tool in glslangValidator spirv-val; do
  command -v "$tool" >/dev/null || { echo "run_hdr_tests: $tool not found" >&2; exit 1; }
done
OUT="$ROOT/build/tests/hdr"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" -I/opt/homebrew/include "$ROOT/tests/runtime/test_hdr_output.cpp" -o "$OUT/test_hdr_output"
status=0
perl -e 'alarm 30; exec @ARGV' "$OUT/test_hdr_output" "$OUT" || status=$?
if [ "$status" = 0 ]; then
  if perl -e 'alarm 60; exec @ARGV' glslangValidator -V --target-env vulkan1.0 -S comp -o "$OUT/hdr.spv" "$OUT/hdr.comp" >"$OUT/glslang.txt" 2>&1 &&
     perl -e 'alarm 60; exec @ARGV' spirv-val --target-env vulkan1.0 "$OUT/hdr.spv"; then
    echo "pass the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
  else
    cat "$OUT/glslang.txt"
    echo "FAIL the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
    status=1
  fi
fi
echo "hdr tests: $([ "$status" = 0 ] && echo PASS || echo FAIL)"
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
