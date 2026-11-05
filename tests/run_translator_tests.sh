#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ForzaRecomp contributors; FM4 Recomp contributors
#
# Taken from ForzaRecomp tests/run_translator_tests.sh (ForzaRecomp). FM4 Recomp changes:
# preflight first; the EXPERIMENTAL frsqrte section is left out until patch 0031 is imported.
# Fixtures in tests/translator/ are ForzaRecomp's, copied unchanged per imported patch (each
# names its patch and derives its expectations from the PowerPC ISA by hand).
#
# Translator tests (T0). Needs scripts/setup_tools.sh first, plus LLVM's llvm-mc and llvm-objdump
# and lld (LLVM=<bin dir>, LLD=<ld.lld>).
#
#   tests/run_translator_tests.sh [name ...]
#
# With names (fixture basenames without .s), only those fixtures run: the focused run used per
# change. Without names, every fixture runs: the full suite, for milestone close and large steps.
#
# tests/translator/*.s          must translate with no fatal problem; XenonRecomp's test mode then
#                               generates a harness from the "#_ REGISTER_IN/OUT" directives, which
#                               is compiled and run: any "EXPECTED ... ACTUAL" line is a failure.
#                               Mutation control: a copy of each file with every REGISTER_OUT and
#                               MEMORY_OUT value changed must fail every generated check.
#                               Upstream harness parser limits, enforced here: every label must be
#                               a test_ label with an expectation (a label without one swallows the
#                               next label; caught by the "harness runs N of M tests" check); plain
#                               "#" comments are stripped from the harness copy (inside a test they
#                               would detach the expectations after them); REGISTER_OUT on cr or
#                               ctr is rejected (the parser ignores registers starting with "c"):
#                               copy them to a GPR instead; so is a label directly
#                               after an expectation line (the parser consumes it).
#                               "#! EMITS <text>" lines name text the generated code must contain
#                               (for effects a single-threaded run cannot observe, e.g. fences).
# tests/translator/trap/*.s     must translate, and the harness must stop at a trap (a signal)
#                               before printing anything.
# tests/translator/reject/*.s   must fail to translate with the FATAL message named on its
#                               "#! FATAL <text>" line.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
XR="$ROOT/tools/XenonRecomp"
WORK="$ROOT/build/tests/translator"
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
  else LLVM="$(dirname "$(command -v clang)")"; fi
fi
LLD="${LLD:-$(command -v ld.lld || echo "$LLVM/ld.lld")}"
for tool in "$LLVM/llvm-mc" "$LLVM/llvm-objdump" "$LLVM/clang++" "$LLD" "$XR/build/XenonRecomp/XenonRecomp"; do
  [ -x "$tool" ] || { echo "missing $tool"; exit 1; }
done
rm -rf "$WORK" && mkdir -p "$WORK"
FOCUS=" $* "
selected() { [ "$FOCUS" = "  " ] || [[ "$FOCUS" == *" $(basename "$1" .s) "* ]]; }
for n in "$@"; do
  compgen -G "$ROOT/tests/translator/$n.s" >/dev/null || compgen -G "$ROOT/tests/translator/*/$n.s" >/dev/null \
    || { echo "FAIL $n: no such fixture"; exit 1; }
done

# Assemble <src>.s into <dir>/bin/<name>.o and .dis, as XenonRecomp's test mode expects
# (<dir>/<name>.s next to <dir>/bin/). Register names gain llvm-mc's % prefix.
assemble() {
  local src="$1" dir="$2" name
  name="$(basename "$src" .s)"
  mkdir -p "$dir/bin"
  grep -v '^[[:space:]]*#[^_!]' "$src" | grep -v '^[[:space:]]*#$' > "$dir/$name.s"
  perl -pe 'next if /^\s*#/; s/\b(r|v|f|cr)(\d+)\b/%$1$2/g unless /:\s*$/' "$src" > "$dir/bin/$name.llvm.s"
  "$LLVM/llvm-mc" -triple=powerpc-unknown-unknown -mcpu=pwr7 -mattr=+altivec -filetype=obj \
    -o "$dir/bin/$name.obj" "$dir/bin/$name.llvm.s"
  "$LLD" -o "$dir/bin/$name.o" -e 0 --image-base=0 -Ttext=0x10000 "$dir/bin/$name.obj"
  "$LLVM/llvm-objdump" -d "$dir/bin/$name.o" > "$dir/bin/$name.dis"
  rm "$dir/bin/$name.obj" "$dir/bin/$name.llvm.s"
}

# Translate <dir>/bin into <dir>/out, compile the harness, run it; prints its output.
build_and_run() {
  local dir="$1"
  mkdir -p "$dir/out"
  "$XR/build/XenonRecomp/XenonRecomp" "$dir/bin" "$dir/out" > "$dir/recomp.log" 2>&1 || {
    cat "$dir/recomp.log"; echo "translation failed: $dir"; return 1; }
  "$LLVM/clang++" -std=c++20 -O1 -fwrapv -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable \
    -Werror=asm-operand-widths \
    -include cstdlib -include "$ROOT/tests/translator/harness_doubles.h" -I"$XR/XenonUtils" -I"$XR/thirdparty/simde" -I"$XR/thirdparty/fmt/include" \
    "$dir"/out/*.cpp "$XR/build/thirdparty/fmt/libfmt.a" -o "$dir/harness"
  "$dir/harness"
}

status=0
positive=0
for src in "$ROOT"/tests/translator/*.s "$ROOT"/tests/translator/trap/*.s; do
  [ -e "$src" ] || continue
  selected "$src" || continue
  if grep -q '^[[:space:]]*#_ REGISTER_OUT c' "$src"; then
    echo "FAIL $(basename "$src"): REGISTER_OUT on cr or ctr is not checked by the harness"; status=1; continue
  fi
  if grep -A1 '^[[:space:]]*#_ \(REGISTER\|MEMORY\)_OUT' "$src" | grep -q '^[A-Za-z0-9_.]*:'; then
    echo "FAIL $(basename "$src"): a label directly after an expectation is skipped by the harness; add a blank line"; status=1
  fi
done

for src in "$ROOT"/tests/translator/*.s; do
  [ -e "$src" ] || continue
  selected "$src" || continue
  name="$(basename "$src" .s)"
  dir="$WORK/$name"
  assemble "$src" "$dir"
  tests=$(grep -c '^test_[A-Za-z0-9_]*:' "$src")
  output="$(build_and_run "$dir")" || { echo "$output" | tail -20; echo "FAIL $name: translation, build or run failed"; status=1; continue; }
  called=$(grep -c '^void test_' "$dir/out/main.cpp" || true)
  missing=""
  while IFS= read -r want; do
    grep -qF -- "$want" "$dir"/out/"$name".cpp || missing="$missing [$want]"
  done < <(sed -n 's/^#! EMITS //p' "$src")
  if [ -n "$missing" ]; then
    echo "FAIL $name: generated code lacks$missing"; status=1
  elif [ "$called" != "$tests" ]; then
    echo "FAIL $name: harness runs $called of $tests tests"; status=1
  elif grep -q 'EXPECTED' <<< "$output"; then
    echo "FAIL $name:"; grep 'EXPECTED' <<< "$output"; status=1
  else
    echo "pass $name: $tests tests"; positive=$((positive + tests))
  fi
  # Mutation control: every expectation changed; every generated check must then fail.
  mutant="$WORK/${name}_mutant"
  perl -pe 'if (/^\s*#_ REGISTER_OUT v/) { s/\[[^\]]*\]/[5A5A5A5B, 5A5A5A5B, 5A5A5A5B, 5A5A5A5B]/ }
            elsif (/^\s*#_ REGISTER_OUT \S+ \S*\./) { s/(REGISTER_OUT \S+ )\S+/${1}12345.25/ }
            elsif (/^\s*#_ REGISTER_OUT/) { s/(REGISTER_OUT \S+ )\S+/${1}0x5A5A5A5A5B/ }
            elsif (/^\s*#_ MEMORY_OUT/) { s/(MEMORY_OUT \S+ )(.*)/my ($p, $b) = ($1, $2); $p . ($b =~ s{[0-9A-Fa-f]{2}}{A5}gr)/e }' \
    "$src" > "$WORK/${name}_mutant.s"
  assemble "$WORK/${name}_mutant.s" "$mutant"
  failed=$(build_and_run "$mutant" | grep -c 'EXPECTED' || true)
  checks=$(grep -c $'^\tPPC_CHECK_VALUE_' "$mutant/out/main.cpp" || true)
  directives=$(grep -c '^[[:space:]]*#_ \(REGISTER\|MEMORY\)_OUT' "$src" || true)
  if [ "$checks" -ge "$directives" ] && [ "$failed" = "$checks" ]; then
    echo "  mutation control: $failed of $checks checks detected"
  else
    echo "FAIL $name: mutation control detected $failed of $checks checks ($directives expectations)"; status=1
  fi
done

trapped=0
for src in "$ROOT"/tests/translator/trap/*.s; do
  [ -e "$src" ] || continue
  selected "$src" || continue
  name="$(basename "$src" .s)"
  dir="$WORK/trap/$name"
  assemble "$src" "$dir"
  set +e; output="$(build_and_run "$dir" 2>&1)"; code=$?; set -e
  if [ "$code" -gt 128 ] && ! grep -q 'EXPECTED' <<< "$output"; then
    echo "pass $name: stopped by signal $((code - 128))"; trapped=$((trapped + 1))
  else
    echo "FAIL $name: expected the harness to stop at a trap (exit $code)"; echo "$output" | tail -3; status=1
  fi
done

rejected=0
for src in "$ROOT"/tests/translator/reject/*.s; do
  [ -e "$src" ] || continue
  selected "$src" || continue
  name="$(basename "$src" .s)"
  dir="$WORK/reject/$name"
  want="$(sed -n 's/^#! FATAL //p' "$src")"
  [ -n "$want" ] || { echo "FAIL $name: no '#! FATAL <text>' line"; status=1; continue; }
  assemble "$src" "$dir"
  mkdir -p "$dir/out"
  if "$XR/build/XenonRecomp/XenonRecomp" "$dir/bin" "$dir/out" > "$dir/recomp.log" 2>&1; then
    echo "FAIL $name: translated without a fatal problem"; status=1
  elif grep -q "^FATAL: .*$want" "$dir/recomp.log"; then
    echo "pass $name: rejected ($want)"; rejected=$((rejected + 1))
  else
    echo "FAIL $name: expected FATAL containing '$want'"; grep '^FATAL' "$dir/recomp.log" || true; status=1
  fi
done

echo "translator tests: $positive positive tests, $trapped traps, $rejected rejections, status $status"
exit $status
