#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Build the native FM4 Recomp targets (M2).
#
#   scripts/build.sh <evidence dir>
#
# Runs scripts/preflight.sh first (the machine must be free), pins the SDK (SDKROOT, logged: Homebrew
# clang with the wrong SDK leaves std::__hash_memory undefined; KB general/process.md), configures
# build/native with Ninja, and records into <evidence dir>: wall time, each compile's peak RSS and
# time (compiler launcher scripts/measure_tu.sh), the combined compiler RSS sampled every 0.5 s,
# and the SHA-256 of every built artefact. Bounded: the build is stopped after 1800 s.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -eq 1 ] || { echo "usage: scripts/build.sh <evidence dir>" >&2; exit 2; }
OUT="$1"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
"$ROOT/scripts/preflight.sh"
LLVM=/opt/homebrew/opt/llvm/bin
export SDKROOT="$(xcrun --show-sdk-path)"
{
  echo "date $(date '+%Y-%m-%d %H:%M:%S')"
  echo "SDKROOT $SDKROOT $(xcrun --show-sdk-version)"
  "$LLVM/clang++" --version | head -1
  echo "cmake $(cmake --version | head -1)"
  echo "ninja $(ninja --version)"
  echo "cores $(sysctl -n hw.ncpu), memory $(($(sysctl -n hw.memsize) / 1048576)) MiB"
} > "$OUT/env.txt"
cat "$OUT/env.txt"
: > "$OUT/tu.tsv"
cmake -S "$ROOT" -B "$ROOT/build/native" -G Ninja -Wno-dev \
  -DCMAKE_CXX_COMPILER="$LLVM/clang++" -DCMAKE_OSX_SYSROOT="$SDKROOT" \
  -DCMAKE_CXX_COMPILER_LAUNCHER="$ROOT/scripts/measure_tu.sh;$OUT/tu.tsv" > "$OUT/configure.log"

# Combined compiler RSS, sampled until the build ends.
( peak=0
  while :; do
    rss=$(ps -A -o rss=,comm= | awk '/clang/ {s += $1} END {print s + 0}')
    [ "$rss" -gt "$peak" ] && peak=$rss && echo "$peak" > "$OUT/peak_combined_kib.txt"
    sleep 0.5
  done ) &
SAMPLER=$!
trap 'kill "$SAMPLER" 2>/dev/null || true' EXIT
start=$(perl -MTime::HiRes=time -e 'printf "%.1f", time')
status=0
perl -e 'alarm 1800; exec @ARGV' ninja -C "$ROOT/build/native" > "$OUT/ninja.log" 2>&1 || status=$?
end=$(perl -MTime::HiRes=time -e 'printf "%.1f", time')
{ kill "$SAMPLER" && wait "$SAMPLER"; } 2>/dev/null || true
wall=$(perl -e "printf '%.1f', $end - $start")
peak=$(cat "$OUT/peak_combined_kib.txt" 2>/dev/null || echo 0)
{
  echo "status $status"
  echo "wall_seconds $wall"
  echo "peak_combined_compiler_rss_mib $((peak / 1024))"
  awk -F'\t' 'NR > 0 {n++; t += $2; if ($3 > m) {m = $3; f = $1}} END {printf "compiles %d, cpu_seconds %.1f, max_tu_rss_mib %d (%s)\n", n, t, m / 1048576, f}' "$OUT/tu.tsv"
} | tee "$OUT/summary.txt"
find "$ROOT/build/native" -maxdepth 2 -type f \( -name '*.a' -o -perm -u+x -a ! -name '*.sh' \) ! -path '*/CMakeFiles/*' \
  -exec shasum -a 256 {} + | sed "s|$ROOT/||" | tee "$OUT/artefacts.sha256"
exit $status
