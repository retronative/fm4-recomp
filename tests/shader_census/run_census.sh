#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Offline shader census (M4 item 3), CPU only: export every distinct shader of
# game/files/Media/Shaders (survey/shader_export.py), build tests/shader_census/census.cpp
# against runtime/shader/shader_glsl.cpp, translate every shader and validate it with
# glslangValidator -> spirv-val -> spirv-cross --msl.
#
#   tests/shader_census/run_census.sh <evidence dir>
#   tests/shader_census/run_census.sh <evidence dir> --runtime <dump dir>
#
# --runtime: census the shaders a run loaded instead of the disc's (fm4recomp with FM4_CP_TRACE and
# FM4_SHADER_DUMP=<dump dir>: <vs|ps>_<hash>.bin files); they are copied into export/ with a counts-only index.json.
#
# <evidence dir> must not exist (use research/m4/...). It receives export/ (the microcode dump:
# game bytes, git-ignored), census (the binary), report.tsv, summary.txt, census.log and
# .retention.json. Each tool run has a 30 s deadline and the whole census 1800 s. On success the
# passing shaders' GLSL/SPIR-V/MSL are gone (census.cpp removes them one by one) and work/ keeps
# only the failing shaders' GLSL, SPIR-V and tool logs; it is removed when empty. On failure
# everything stays.
set -euo pipefail

if [[ $# -ne 1 && ! ( $# -eq 3 && $2 == --runtime ) ]]; then
    echo "usage: $0 <evidence dir> [--runtime <dump dir>]" >&2
    exit 2
fi
EVIDENCE=$1
RUNTIME_DUMP=${3:-}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SHADERS=$ROOT/game/files/Media/Shaders
PYTHON=/opt/homebrew/bin/python3
CXX=/opt/homebrew/opt/llvm/bin/clang++
TOOL_DEADLINE=30
CENSUS_DEADLINE=1800

"$ROOT/scripts/preflight.sh" >/dev/null  # the machine must be free (run safety)
if [[ -e $EVIDENCE ]]; then
    echo "refusing: $EVIDENCE exists" >&2
    exit 1
fi
[[ -d $SHADERS ]] || { echo "missing shader directory $SHADERS" >&2; exit 1; }
[[ -f $ROOT/runtime/shader/shader_glsl.cpp ]] || { echo "missing $ROOT/runtime/shader/shader_glsl.cpp" >&2; exit 1; }
for tool in "$PYTHON" "$CXX"; do
    [[ -x $tool ]] || { echo "missing $tool" >&2; exit 1; }
done
GLSLANG=$(command -v glslangValidator) || { echo "missing glslangValidator" >&2; exit 1; }
SPIRV_VAL=$(command -v spirv-val) || { echo "missing spirv-val" >&2; exit 1; }
SPIRV_CROSS=$(command -v spirv-cross) || { echo "missing spirv-cross" >&2; exit 1; }

mkdir -p "$EVIDENCE"
EVIDENCE=$(cd "$EVIDENCE" && pwd)
printf '{"owner": "fm4-recomp/m4-shader-census", "project": "fm4-recomp", "host": "%s", "class": "evidence", "status": "active",\n "created_at": "%s", "pids": [], "lock": null, "note": "M4 shader census: microcode export (game bytes), report.tsv, summary.txt"}\n' \
    "$(hostname -s)" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$EVIDENCE/.retention.json"

{
    echo "started $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    "$CXX" --version | head -1
    "$GLSLANG" --version | head -1
    "$SPIRV_VAL" --version | head -1
    "$SPIRV_CROSS" --revision 2>&1 | head -1 || true
} > "$EVIDENCE/census.log"

if [[ -n $RUNTIME_DUMP ]]; then
    mkdir "$EVIDENCE/export"
    cp "$RUNTIME_DUMP"/vs_*.bin "$RUNTIME_DUMP"/ps_*.bin "$EVIDENCE/export/" 2>/dev/null || true
    vs=$(ls "$EVIDENCE/export" | grep -c '^vs_' || true); ps=$(ls "$EVIDENCE/export" | grep -c '^ps_' || true)
    printf '{"counts": {"ps": %d, "vs": %d}, "format": 1, "source": "runtime dump"}\n' "$ps" "$vs" > "$EVIDENCE/export/index.json"
    echo "runtime dump $RUNTIME_DUMP: vs $vs ps $ps" | tee -a "$EVIDENCE/census.log"
else
    "$PYTHON" -B -I "$ROOT/survey/shader_export.py" "$SHADERS" "$EVIDENCE/export" | tee -a "$EVIDENCE/census.log"
fi

# The translator and the census are compiled separately: -Werror applies to both, and a
# translator warning fails the run rather than being silenced.
FLAGS=(-std=c++20 -O2 -Wall -Wextra -Werror -I "$ROOT/runtime")
"$CXX" "${FLAGS[@]}" -c "$ROOT/runtime/shader/shader_glsl.cpp" -o "$EVIDENCE/shader_glsl.o"
"$CXX" "${FLAGS[@]}" -c "$ROOT/tests/shader_census/census.cpp" -o "$EVIDENCE/census.o"
"$CXX" "$EVIDENCE/census.o" "$EVIDENCE/shader_glsl.o" -o "$EVIDENCE/census"
rm -f "$EVIDENCE/census.o" "$EVIDENCE/shader_glsl.o"
echo "census sha256 $(shasum -a 256 "$EVIDENCE/census" | cut -d' ' -f1)" | tee -a "$EVIDENCE/census.log"

mkdir "$EVIDENCE/work"
start=$(date +%s)
# perl's alarm bounds the whole census (SIGALRM kills it; macOS has no timeout(1)).
/usr/bin/perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$CENSUS_DEADLINE" \
    "$EVIDENCE/census" "$EVIDENCE/export" "$EVIDENCE/work" "$EVIDENCE/report.tsv" "$EVIDENCE/summary.txt" \
    "$GLSLANG" "$SPIRV_VAL" "$SPIRV_CROSS" "$TOOL_DEADLINE" | tee -a "$EVIDENCE/census.log"
echo "census wall time $(( $(date +%s) - start )) s, finished $(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$EVIDENCE/census.log"

rmdir "$EVIDENCE/work" 2>/dev/null || echo "work/ keeps the failing shaders' files: $EVIDENCE/work"
