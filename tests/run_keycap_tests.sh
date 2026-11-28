#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp (FM4_KEYCAP_ICONS): the keycap prompt icons (runtime/video/keycap.h; tests/runtime/test_keycap.cpp), CPU only.
# Built with warnings as errors under ASan/UBSan; a passing run removes its work directory. KEYCAP_PNG_DIR=<dir> writes the caps.
#
#   tests/run_keycap_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
LLVM=/opt/homebrew/opt/llvm/bin
OUT="$ROOT/build/tests/keycap"
mkdir -p "$OUT"
export SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path)}"
perl -e 'alarm 300; exec @ARGV' "$LLVM/clang++" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isysroot "$SDKROOT" \
  -I"$ROOT/runtime/video" "$ROOT/tests/runtime/test_keycap.cpp" "$ROOT/runtime/video/keycap.cpp" "$ROOT/runtime/video/keycap_text_mac.cpp" \
  -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework ImageIO -o "$OUT/test_keycap"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_keycap" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
