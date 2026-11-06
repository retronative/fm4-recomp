#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Xenos microcode -> GLSL translator T0 tests (tests/shader/test_shader_glsl.cpp), CPU only: every
# emitted shader is compiled with glslangValidator and checked with spirv-val; no GPU device is
# created. Built with warnings as errors.
#
#   tests/shader/run_shader_tests.sh
#
# WORK_DIR=<dir> puts the binary and the emitted shaders there (default build/tests/shader); a
# passing run removes it, a failing one keeps it (KEEP_WORK=1 keeps it either way).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
export PATH="/opt/homebrew/bin:$PATH"
for tool in glslangValidator spirv-val; do
  command -v "$tool" >/dev/null || { echo "run_shader_tests: $tool not found" >&2; exit 1; }
done
OUT="${WORK_DIR:-$ROOT/build/tests/shader}"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -Wall -Wextra -Wpedantic -Werror -isysroot "$SDKROOT" \
  -I"$ROOT/runtime" "$ROOT/tests/shader/test_shader_glsl.cpp" "$ROOT/runtime/shader/shader_glsl.cpp" \
  -o "$OUT/test_shader_glsl"
status=0
perl -e 'alarm 300; exec @ARGV' "$OUT/test_shader_glsl" "$OUT/glsl" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
