#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Re-checks LICENSES/SOURCES.tsv (read-only): every licence text under LICENSES/ still equals the local source it was copied from
# (the whole file, or the stated line range, in tools/ and the release dependencies' sources), both recorded SHA-256 values
# still hold, and no file under LICENSES/ is missing from the table (Xenia-BSD-3-Clause.txt predates it). Run after a pin change.
#   scripts/release/verify_licence_sources.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT"
fail=0; n=0
while IFS=$'\t' read -r draft source lines sha_draft sha_source rest; do
  case "$draft" in '#'*|path) continue ;; esac
  n=$((n + 1))
  case "$source" in '$ROOT'*) ;; *) [ "$(shasum -a 256 "$HERE/$draft" | cut -d' ' -f1)" = "$sha_draft" ] || { echo "FAIL $draft: SHA-256"; fail=1; }; continue ;; esac  # no local source: the hash only
  src="${source/\$ROOT/$ROOT}"
  [ -f "$src" ] || { echo "FAIL $draft: source $source is missing"; fail=1; continue; }
  if [ "$lines" = all ]; then cmp -s "$src" "$HERE/$draft" || { echo "FAIL $draft: differs from $source"; fail=1; }
  else sed -n "${lines%-*},${lines#*-}p" "$src" | cmp -s - "$HERE/$draft" || { echo "FAIL $draft: differs from $source lines $lines"; fail=1; }; fi
  [ "$(shasum -a 256 "$HERE/$draft" | cut -d' ' -f1)" = "$sha_draft" ] || { echo "FAIL $draft: SHA-256 of the text"; fail=1; }
  [ "$(shasum -a 256 "$src" | cut -d' ' -f1)" = "$sha_source" ] || { echo "FAIL $draft: SHA-256 of the source"; fail=1; }
done < "$HERE/LICENSES/SOURCES.tsv"
files="$(find "$HERE/LICENSES" -type f ! -name SOURCES.tsv ! -name Xenia-BSD-3-Clause.txt | wc -l | tr -d ' ')"
[ "$files" = "$n" ] || { echo "FAIL: $files files under LICENSES/, $n rows in SOURCES.tsv"; fail=1; }
[ $fail = 0 ] && echo "verify_sources: $n files match their sources"
exit $fail
