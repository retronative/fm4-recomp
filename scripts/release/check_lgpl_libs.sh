#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The kit's LGPL-2.1 parts (FFmpeg's libavcodec and libavutil, libmspack's LZX decoder) are separate, replaceable libraries in
# lib/, never inside a program. After ReCarbon scripts/macos/check_app.py's check 7 (GPL-3.0-or-later); FM4 Recomp changes:
# the kit layout (bin/, lib/), FM4's library names, a shell script. Fails, naming each problem, unless:
#   - no program in bin/ defines an FFmpeg (av*, ff_*) or LZX decoder (lzxd_*) function;
#   - each library is in lib/, is the one it is named as (it defines that library's function) and is named @rpath/<file>;
#   - bin/fm4recomp loads all three by @rpath/<file>, and bin/xexsurvey the LZX decoder.
#   scripts/release/check_lgpl_libs.sh <kit dir>
# tests/test_lgpl_libs.py runs it on stand-in kits with planted faults.
set -euo pipefail
[ $# = 1 ] || { echo "usage: scripts/release/check_lgpl_libs.sh <kit dir>" >&2; exit 2; }
KIT="$1"
fail=0
bad() { echo "check_lgpl_libs: $*" >&2; fail=1; }
defines() { nm -gU "$1" 2>/dev/null | awk '{print $NF}' | grep -E "$2" || true; }  # the external symbols <file> defines that match

for f in "$KIT"/bin/*; do
  inside="$(defines "$f" '^_(lzxd_|av_|avcodec_|avutil_|ff_)' | head -3 | tr '\n' ' ')"
  [ -z "$inside" ] || bad "$(basename "$f") contains LGPL library code ($inside...)"
done
for entry in libavcodec.61.dylib:_avcodec_send_packet libavutil.59.dylib:_av_frame_alloc libfm4_lzx.dylib:_lzxd_decompress; do
  name="${entry%%:*}"; symbol="${entry##*:}"
  lib="$KIT/lib/$name"
  [ -f "$lib" ] || { bad "lib/$name is missing"; continue; }
  [ -n "$(defines "$lib" "^$symbol\$")" ] || bad "lib/$name does not define ${symbol#_}"
  [ "$(otool -D "$lib" | tail -1)" = "@rpath/$name" ] || bad "lib/$name is not named @rpath/$name"
done
loads() { otool -L "$1" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep -qx "@rpath/$2"; }
for name in libavcodec.61.dylib libavutil.59.dylib libfm4_lzx.dylib; do
  loads "$KIT/bin/fm4recomp" "$name" || bad "bin/fm4recomp does not load @rpath/$name"
done
loads "$KIT/bin/xexsurvey" libfm4_lzx.dylib || bad "bin/xexsurvey does not load @rpath/libfm4_lzx.dylib"
[ $fail = 0 ] || exit 1
echo "check_lgpl_libs: the LGPL parts are separate libraries in $KIT/lib"
