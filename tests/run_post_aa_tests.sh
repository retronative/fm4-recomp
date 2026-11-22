#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# runtime/video/post_aa.h (M9, FM4_POST_AA), CPU only, ASan/UBSan, warnings as errors: the switch's parse, the dispatch's workgroup
# counts, and the emitted compute shader through glslangValidator + spirv-val (the runtime compiles the same text with glslang's
# library for Vulkan 1.0 / SPIR-V 1.0). No GPU. A passing run removes its work directory.
#
#   tests/run_post_aa_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
export PATH="/opt/homebrew/bin:$PATH"
for tool in glslangValidator spirv-val; do
  command -v "$tool" >/dev/null || { echo "run_post_aa_tests: $tool not found" >&2; exit 1; }
done
OUT="$ROOT/build/tests/post_aa"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 120; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_post_aa.cpp" -o "$OUT/test_post_aa"
status=0
perl -e 'alarm 30; exec @ARGV' "$OUT/test_post_aa" "$OUT" || status=$?
if [ "$status" = 0 ]; then
  if perl -e 'alarm 60; exec @ARGV' glslangValidator -V --target-env vulkan1.0 -S comp -o "$OUT/post_aa.spv" "$OUT/post_aa.comp" >"$OUT/glslang.txt" 2>&1 &&
     perl -e 'alarm 60; exec @ARGV' spirv-val --target-env vulkan1.0 "$OUT/post_aa.spv"; then
    echo "pass the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
  else
    cat "$OUT/glslang.txt"
    echo "FAIL the emitted compute shader compiles (glslangValidator) and validates (spirv-val)"
    status=1
  fi
fi
echo "post aa tests: $([ "$status" = 0 ] && echo PASS || echo FAIL)"
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
