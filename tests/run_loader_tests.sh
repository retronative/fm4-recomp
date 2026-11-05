#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors; FM4 Recomp contributors
#
# XEX loader regression tests. Needs scripts/setup_tools.sh first.
#
#   tests/run_loader_tests.sh [name ...]
#
# With names (test basenames without .cpp), only those tests run (focused); without, all of them.
#
# Builds tests/loader/*_test.cpp with AddressSanitizer against the toolchain's patched XenonUtils
# and runs them. Negative control: each test, built with the loader source from the pinned upstream
# base (before our loader patches), must fail with the signal its "// Negative control:" line
# names: FAIL (checks fail, no sanitizer report) or AddressSanitizer (the old loader overruns).
# ASan fills whole allocations with garbage, so a missing zero fill is detected.
# XEX_CPP=<file> builds the positive runs against that loader source instead of the library
# (used by the mutant runner; the negative controls are unchanged).
#
# Taken from ForzaRecomp tests/run_loader_tests.sh (ForzaRecomp). FM4 Recomp changes: the
# upstream pin is read from scripts/setup_tools.sh, preflight runs first, XEX_CPP override.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
XR="$ROOT/tools/XenonRecomp"
OUT="$ROOT/build/tests/loader"
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
  else LLVM="$(dirname "$(command -v clang)")"; fi
fi
BASE="$(sed -n 's/^XENONRECOMP_COMMIT=//p' "$ROOT/scripts/setup_tools.sh")"
[ -n "$BASE" ] || { echo "run_loader_tests: no XENONRECOMP_COMMIT in scripts/setup_tools.sh" >&2; exit 1; }
mkdir -p "$OUT"
INCLUDES=(-I"$XR/XenonUtils" -I"$XR/thirdparty" -I"$XR/thirdparty/fmt/include" -I"$XR/thirdparty/xxHash"
          -I"$XR/thirdparty/TinySHA1" -I"$XR/thirdparty/libmspack/libmspack/mspack" -I"$XR/thirdparty/tiny-AES-c")
LIBS=("$XR/build/XenonUtils/libXenonUtils.a" "$XR/build/thirdparty/fmt/libfmt.a"
      "$XR/build/thirdparty/disasm/libdisasm.a" "$XR/build/thirdparty/xxHash/cmake_unofficial/libxxhash.a")
FLAGS=(-std=c++20 -O1 -g -include cstdlib -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
POSITIVE=()
[ -n "${XEX_CPP:-}" ] && POSITIVE=("$XEX_CPP")

export ASAN_OPTIONS="max_malloc_fill_size=1048576${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
git -C "$XR" show "$BASE:XenonUtils/xex.cpp" > "$OUT/xex_base.cpp"
status=0
FOCUS=" $* "
for n in "$@"; do [ -e "$ROOT/tests/loader/$n.cpp" ] || { echo "FAIL: $n: no such test"; exit 1; }; done
for test in "$ROOT"/tests/loader/*_test.cpp; do
  name="$(basename "$test" .cpp)"
  [ "$FOCUS" = "  " ] || [[ "$FOCUS" == *" $name "* ]] || continue
  "$LLVM/clang++" "${FLAGS[@]}" "${INCLUDES[@]}" "$test" ${POSITIVE[@]+"${POSITIVE[@]}"} "${LIBS[@]}" -o "$OUT/$name"
  if "$OUT/$name" > "$OUT/$name.log" 2>&1; then
    echo "pass: $name ($(tail -1 "$OUT/$name.log"))"
  else
    echo "FAIL: $name"; sed 's/^/  /' "$OUT/$name.log" | head -20; status=1
  fi
  [ -n "${XEX_CPP:-}" ] && continue
  # Negative control: the same test against the unpatched upstream loader must fail.
  "$LLVM/clang++" "${FLAGS[@]}" "${INCLUDES[@]}" "$test" "$OUT/xex_base.cpp" "${LIBS[@]}" -o "$OUT/${name}_base"
  want="$(sed -n 's|^// Negative control: ||p' "$test")"
  log="$OUT/${name}_base.log"
  if "$OUT/${name}_base" > "$log" 2>&1; then
    echo "NEGATIVE CONTROL FAILED: $name passes against the unpatched loader"; status=1
  elif [ "$want" = FAIL ] && grep -q '^FAIL ' "$log" && ! grep -q 'Sanitizer' "$log"; then
    echo "negative control: $name fails against the unpatched loader ($(grep -c '^FAIL ' "$log") checks)"
  elif [ "$want" = AddressSanitizer ] && grep -q '^==[0-9]*==ERROR: AddressSanitizer' "$log"; then
    echo "negative control: $name overruns in the unpatched loader ($(grep -m1 -o 'AddressSanitizer: [a-z-]*' "$log"))"
  else
    echo "NEGATIVE CONTROL FAILED: $name: expected '$want' from the unpatched loader, see $log"; status=1
  fi
done
exit $status
