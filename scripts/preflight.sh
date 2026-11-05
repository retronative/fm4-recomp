#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Refuse to start a run unless the machine is free. Exit 0: free. Exit 1: busy (each reason
# printed). Launchers call it first and stop on a non-zero exit; it never only warns.
#
#   scripts/preflight.sh [--gpu]
#
# Busy when any of these run: a build (ninja, cmake --build, clang over a large file set), the
# recompiler, ForzaRecomp's or this project's game binaries, a headless Chrome (another
# project's browser tests), a benchmark. With --gpu also busy when research/GPU_STOP exists
# (set by the watchdog after a GPU lockup; only the owner removes it).
# Patterns match executable paths and command lines, not project names in an editor's arguments.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
busy=0
check() {  # check <reason> <pgrep -f pattern>
  local found
  found="$(pgrep -fl "$2" 2>/dev/null | grep -v -e "preflight.sh" || true)"
  if [ -n "$found" ]; then
    echo "preflight: BUSY: $1" >&2
    printf '%s\n' "$found" | head -5 | sed 's/^/  /' >&2
    busy=1
  fi
}
check "a build is running" '(^|/)(ninja|make)( |$)|cmake --build'
check "the recompiler is running" '(^|/)(XenonRecomp|XenonAnalyse)( |$)'
check "a ForzaRecomp binary is running (never run alongside it)" '/forzarecomp[^ ]*/build/[^ ]*'
check "an FM4 Recomp binary is running" "$ROOT/build/[^ ]*"
check "a headless Chrome is running (another project's GPU work)" 'Chrome.*--headless'
if [ "${1:-}" = --gpu ] && [ -e "$ROOT/research/GPU_STOP" ]; then
  echo "preflight: BUSY: research/GPU_STOP is set: no GPU run until the owner removes it" >&2
  sed 's/^/  /' "$ROOT/research/GPU_STOP" >&2 || true
  busy=1
fi
[ $busy = 0 ] && echo "preflight: free"
exit $busy
