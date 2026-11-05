# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Helpers for tests/run_translator_tests.sh (sourced), tested by tests/test_translator_lib.py
# (which runs this file as a command: translator_lib.sh <function> <args...>).
#
# Harness cache. A harness binary is a function of: the generated code (every file XenonRecomp's
# test mode wrote), every header it can include (XenonUtils, simde, fmt), the doubles header,
# libfmt.a, the compiler and its flags, the SDK, and this runner. The key hashes all of them, so
# a mutant that changes any fixture's generated code, or any header, misses the cache; one that
# changes nothing a fixture compiles gets the identical binary it would have built. The cached
# binary is always executed: the test still runs.

# cache_tool_key <XR> <doubles.h> <clang++> <runner> -- <flags...>: everything but the generated code.
cache_tool_key() {
  local xr="$1" doubles="$2" cxx="$3" runner="$4"
  shift 4; [ "$1" = "--" ] && shift
  {
    echo "compiler $cxx"; "$cxx" --version
    echo "sdk $(xcrun --show-sdk-path 2>/dev/null) $(xcrun --show-sdk-version 2>/dev/null)"
    echo "flags $*"
    echo "== doubles"; cat "$doubles"
    echo "== runner"; cat "$runner"
    echo "== libfmt"; cat "$xr/build/thirdparty/fmt/libfmt.a"
    (cd "$xr" && find XenonUtils thirdparty/simde thirdparty/fmt/include -type f -print0 \
       | LC_ALL=C sort -z | xargs -0 shasum -a 256)
  } | shasum -a 256 | cut -d' ' -f1
}

# cache_harness_key <tool key> <out dir>: the tool key plus every generated file, name and content.
cache_harness_key() {
  local toolkey="$1" out="$2"
  { echo "tool $toolkey"; (cd "$out" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256); } \
    | shasum -a 256 | cut -d' ' -f1
}

# cache_prune <dir> <max KiB>: size-based eviction. Entries (64-hex-digit directories) are kept
# newest first while their total size stays within the cap; older ones are removed. Nothing else in
# <dir> is touched.
cache_prune() {
  local dir="$1" max="$2" total=0 size entry
  [ -d "$dir" ] || return 0
  ls -1t "$dir" | grep -E '^[0-9a-f]{64}$' | while read -r entry; do
    size="$(du -sk "$dir/$entry" | cut -f1)"
    if [ $((total + size)) -le "$max" ]; then
      total=$((total + size))
    else
      rm -r "${dir:?}/$entry"
    fi
  done
}

# bounded <seconds> <cmd...>: run cmd; past the deadline, SIGKILL it and exit 124 with a TIMEOUT
# line on stderr. A signal death exits 128 + signal, as in bash, so a hang (124) can never pass
# for a trap (> 128).
bounded() {
  perl -e '
    my $t = shift;
    my $pid = fork; die "fork: $!" unless defined $pid;
    if (!$pid) { exec @ARGV; die "exec $ARGV[0]: $!\n"; }
    $SIG{ALRM} = sub { kill "KILL", $pid; waitpid $pid, 0; print STDERR "TIMEOUT after ${t}s: @ARGV\n"; exit 124 };
    alarm $t; waitpid $pid, 0; alarm 0;
    my $s = $?; exit($s & 127 ? 128 + ($s & 127) : $s >> 8);' "$@"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail
  "$@"
fi
