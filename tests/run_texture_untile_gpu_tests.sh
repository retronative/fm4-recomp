#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The GPU texture load's CPU side (FM4_TEXTURE_GPU_UNTILE; tests/runtime/test_texture_untile_gpu.cpp, runtime/video/texture_untile_gpu.h):
# the shader's shared body run as C++ against FillStaging and Xenia's tiled offset for every block size, endian, tiled and linear,
# mips and packed origins; the binding arithmetic; the plan's refusals; the load-path choice and the guard's overlap model. The emitted
# compute shader is compiled with glslangValidator and checked with spirv-val. No Vulkan header, no GPU device. Built with warnings as
# errors under ASan/UBSan; a passing run removes its work directory.
#
#   tests/run_texture_untile_gpu_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
export PATH="/opt/homebrew/bin:$PATH"
for tool in glslangValidator spirv-val; do
  command -v "$tool" >/dev/null || { echo "run_texture_untile_gpu_tests: $tool not found" >&2; exit 1; }
done
OUT="$ROOT/build/tests/texture_untile_gpu"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" -I"$ROOT/runtime/video" -I/opt/homebrew/include "$ROOT/tests/runtime/test_texture_untile_gpu.cpp" \
  "$ROOT/runtime/video/texture_untile_gpu.cpp" "$ROOT/runtime/video/texture_upload.cpp" "$ROOT/runtime/video/texture_info.cpp" \
  "$ROOT/runtime/video/texture_untile.cpp" "$ROOT/runtime/video/edram.cpp" "$ROOT/runtime/video/edram_vk_plan.cpp" -o "$OUT/test_texture_untile_gpu"
status=0
perl -e 'alarm 120; exec @ARGV' "$OUT/test_texture_untile_gpu" "$OUT" || status=$?
if [ "$status" = 0 ]; then
  # The runtime compiles the same text with glslang's library for Vulkan 1.0 / SPIR-V 1.0 (shader_cache.cpp CompileCompute).
  if perl -e 'alarm 60; exec @ARGV' glslangValidator -V --target-env vulkan1.0 -S comp -o "$OUT/texture_untile.spv" "$OUT/texture_untile.comp" >"$OUT/glslang.txt" 2>&1 &&
     perl -e 'alarm 60; exec @ARGV' spirv-val --target-env vulkan1.0 "$OUT/texture_untile.spv"; then
    echo "pass the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
  else
    cat "$OUT/glslang.txt"
    echo "FAIL the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
    status=1
  fi
fi
echo "texture untile gpu tests: $([ "$status" = 0 ] && echo PASS || echo FAIL)"
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
