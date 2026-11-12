#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The GPU present's CPU side (FM4_GPU_PRESENT; tests/runtime/test_gpu_present.cpp, runtime/video/gpu_present.h): the shader's shared
# body run as C++ against today's CPU path (the traced front-buffer resolve's write-back + Detile32 + the red/blue exchange +
# ApplyPwlToFrame) for every endian, both swizzles and three ramps; the PWL tables against ApplyPwl10 and hand values; the resolve
# match and the front-buffer tracker. The emitted compute shader is compiled with glslangValidator and checked with spirv-val. No GPU
# device. Built with warnings as errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_gpu_present_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
export PATH="/opt/homebrew/bin:$PATH"
for tool in glslangValidator spirv-val; do
  command -v "$tool" >/dev/null || { echo "run_gpu_present_tests: $tool not found" >&2; exit 1; }
done
OUT="$ROOT/build/tests/gpu_present"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" -I/opt/homebrew/include "$ROOT/tests/runtime/test_gpu_present.cpp" \
  "$ROOT/runtime/video/gpu_present.cpp" "$ROOT/runtime/video/edram.cpp" "$ROOT/runtime/video/edram_vk_plan.cpp" \
  "$ROOT/runtime/kernel/gamma_ramp.cpp" -o "$OUT/test_gpu_present"
status=0
perl -e 'alarm 120; exec @ARGV' "$OUT/test_gpu_present" "$OUT" || status=$?
if [ "$status" = 0 ]; then
  # The runtime compiles the same text with glslang's library for Vulkan 1.0 / SPIR-V 1.0 (shader_cache.cpp CompileCompute).
  if perl -e 'alarm 60; exec @ARGV' glslangValidator -V --target-env vulkan1.0 -S comp -o "$OUT/gpu_present.spv" "$OUT/gpu_present.comp" >"$OUT/glslang.txt" 2>&1 &&
     perl -e 'alarm 60; exec @ARGV' spirv-val --target-env vulkan1.0 "$OUT/gpu_present.spv"; then
    echo "pass the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
  else
    cat "$OUT/glslang.txt"
    echo "FAIL the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
    status=1
  fi
fi
echo "gpu present tests: $([ "$status" = 0 ] && echo PASS || echo FAIL)"
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
