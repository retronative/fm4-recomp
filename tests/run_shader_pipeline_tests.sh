#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The shader cache, class tag, pipeline key and upload-ring tests
# (tests/runtime/test_shader_cache.cpp, tests/runtime/test_pipeline_cache.cpp), CPU only: the glslang
# library compiles GLSL in-process; no Vulkan header, no device. Built with warnings as errors under
# ASan/UBSan; a passing run removes its work directory (KEEP_WORK=1 keeps it).
#
#   tests/run_shader_pipeline_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
GLSLANG_PREFIX=/opt/homebrew/opt/glslang
for f in "$GLSLANG_PREFIX/include/glslang/Public/ShaderLang.h" "$GLSLANG_PREFIX/lib/libglslang.dylib" "$GLSLANG_PREFIX/lib/libglslang-default-resource-limits.dylib"; do
  [ -e "$f" ] || { echo "run_shader_pipeline_tests: $f not found (brew install glslang)" >&2; exit 1; }
done
SRC="${DRAFT:-$ROOT}"
OUT="$ROOT/build/tests/shader_pipeline"
mkdir -p "$OUT"
rm -rf "$OUT/work" && mkdir "$OUT/work"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
FLAGS=(-std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" -I"$SRC/runtime" -I"$ROOT/runtime")
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" "${FLAGS[@]}" -I"$GLSLANG_PREFIX/include" -I"$ROOT/tools/XenonRecomp/thirdparty/TinySHA1" \
  "$SRC/tests/runtime/test_shader_cache.cpp" "$SRC/runtime/video/shader_cache.cpp" "$ROOT/runtime/shader/shader_glsl.cpp" \
  -L"$GLSLANG_PREFIX/lib" -lglslang -lSPIRV -lglslang-default-resource-limits -Wl,-rpath,"$GLSLANG_PREFIX/lib" \
  -o "$OUT/test_shader_cache"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" "${FLAGS[@]}" \
  "$SRC/tests/runtime/test_pipeline_cache.cpp" "$SRC/runtime/video/pipeline_key.cpp" -o "$OUT/test_pipeline_cache"
status=0
# The Homebrew glslang dylib is not ASan-instrumented: it grows the SPIR-V std::vector without the
# container annotations our instrumented TU set, so reading the result trips ASan's container-overflow
# check (a documented false positive when instrumented and uninstrumented code share a container:
# github.com/google/sanitizers/wiki/AddressSanitizerContainerOverflow). Only that check is off.
export ASAN_OPTIONS="detect_container_overflow=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
perl -e 'alarm 120; exec @ARGV' "$OUT/test_shader_cache" "$OUT/work" || status=$?
perl -e 'alarm 60; exec @ARGV' "$OUT/test_pipeline_cache" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
