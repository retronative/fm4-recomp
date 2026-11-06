#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# CMake compiler launcher (scripts/build.sh): run one compile under /usr/bin/time -l and append
# "<source>\t<seconds>\t<peak RSS bytes>" to the log named first. The compile's own exit status and
# diagnostics pass through unchanged.
set -euo pipefail
LOG="$1"; shift
src=""
for a in "$@"; do case "$a" in *.cpp) src="$a" ;; esac; done
t="$(mktemp "${TMPDIR:-/tmp}/measure_tu.XXXXXX")"
status=0
/usr/bin/time -l -o "$t" "$@" || status=$?
secs=$(awk '/ real / {print $1}' "$t")
rss=$(awk '/maximum resident set size/ {print $1}' "$t")
printf '%s\t%s\t%s\n' "$(basename "$src")" "${secs:-0}" "${rss:-0}" >> "$LOG"
rm -f "$t"
exit $status
