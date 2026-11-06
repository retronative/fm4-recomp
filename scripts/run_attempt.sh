#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Bounded launcher (M2): build, then run the native binary <runs> times and record each run's stop.
#
#   scripts/run_attempt.sh <evidence dir> <runs> <deadline seconds> <stop at swap n> <audio: audible|mute>
#
# Rules (run safety): scripts/preflight.sh must say free; the run chains on a
# successful scripts/build.sh and prints the binary's SHA-256; each run has an isolated HOME under
# game/, its own process group and a deadline (SIGTERM, then SIGKILL of the group); runs are
# sequential. Audio (audio policy): "audible" plays through the default device (authorized
# validation runs only); "mute" opens no device. Each run's log must name the mode it was given ("[audio] audible:"
# with the driver and device, or "[audio] mute: no audio device opened"), or the run fails.
# A run passes when it logged "[runtime] calling the title entry" and ended in a strict stop whose
# exit code matches its first "[stop N]" line (codes in runtime/stops.h: 3 import, 4 residual,
# 5 guest fault, 6 indirect call, 7 unimplemented, 10 trap). Every run must end identically.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -eq 5 ] || { echo "usage: scripts/run_attempt.sh <evidence dir> <runs> <deadline seconds> <stop at swap n> <audio: audible|mute>" >&2; exit 2; }
OUT="$1"; RUNS="$2"; DEADLINE="$3"; STOP_AT_SWAP="$4"; AUDIO="$5"
[[ "$AUDIO" == audible || "$AUDIO" == mute ]] || { echo "audio must be audible or mute" >&2; exit 2; }
[[ "$STOP_AT_SWAP" =~ ^[1-9][0-9]*$ ]] || { echo "the swap to stop at must be a positive integer" >&2; exit 2; }
[[ "$RUNS" =~ ^[1-9][0-9]*$ && "$DEADLINE" =~ ^[1-9][0-9]*$ ]] || { echo "runs and deadline must be positive integers" >&2; exit 2; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
"$ROOT/scripts/preflight.sh"
"$ROOT/scripts/build.sh" "$OUT/build"
BIN="$ROOT/build/native/runtime/fm4recomp"
IMAGE="$ROOT/game/survey/image.bin"
XEX="$ROOT/game/files/default.xex"
DISC="$ROOT/game/files"
# The GPU stop flag the runtime checks at its GPU entry points: research/GPU_STOP, or (integration checks only)
# GPU_STOP_PATH, which is recorded in the evidence.
GPU_STOP_FLAG="$ROOT/research/GPU_STOP"
if [ -n "${GPU_STOP_PATH:-}" ]; then GPU_STOP_FLAG="$GPU_STOP_PATH"; fi
echo "gpu stop flag: $GPU_STOP_FLAG$([ -n "${GPU_STOP_PATH:-}" ] && echo ' (GPU_STOP_PATH override)')" | tee "$OUT/gpu_stop_flag.txt"
MODULES="$ROOT/game/modules"
shasum -a 256 "$BIN" "$IMAGE" | sed "s|$ROOT/||" | tee "$OUT/binary.sha256"
echo "audio: $AUDIO (fm4recomp --audio $AUDIO)" | tee "$OUT/audio.txt"
export HOME="$ROOT/game/home"
mkdir -p "$ROOT/game/user-data"  # saved content (kernel/content.h), isolated from the read-only disc
mkdir -p "$HOME"
for i in $(seq 1 "$RUNS"); do
  status=0
  # Own process group; past the deadline (+30 s grace over the runtime's own watchdog): TERM, then KILL.
  perl -e '
    my ($deadline, @cmd) = @ARGV;
    my $pid = fork; die "fork: $!" unless defined $pid;
    if (!$pid) { setpgrp(0, 0); exec @cmd; die "exec: $!\n"; }
    $SIG{ALRM} = sub { kill "TERM", -$pid; sleep 2; kill "KILL", -$pid; waitpid $pid, 0; print STDERR "[launcher] deadline: process group killed\n"; exit 124 };
    alarm $deadline + 30; waitpid $pid, 0; alarm 0;
    my $s = $?; exit($s & 127 ? 128 + ($s & 127) : $s >> 8);' \
    "$DEADLINE" "$BIN" --image "$IMAGE" --xex "$XEX" --disc "$DISC" --disc-map "$ROOT/game/survey/disc1_gdfx.tsv" --modules "$MODULES" --gpu-stop "$GPU_STOP_FLAG" --user-data "$ROOT/game/user-data" --audio "$AUDIO" --renderer headless --stop-at-swap "$STOP_AT_SWAP" --deadline "$DEADLINE" > "$OUT/run$i.out" 2> "$OUT/run$i.err" || status=$?
  echo "$status" > "$OUT/run$i.exit"
  echo "run $i: exit $status, $(grep -m1 '^\[stop ' "$OUT/run$i.err" || echo 'no stop line')"
done
/opt/homebrew/bin/python3 -B -I - "$OUT" "$RUNS" "$AUDIO" <<'PY'
import json, sys
from pathlib import Path
out, runs, audio = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
AUDIO_LINE = {'audible': '[audio] audible: ', 'mute': '[audio] mute: no audio device opened'}[audio]
STOPS = {3: 'import', 4: 'residual', 5: 'guest fault', 6: 'indirect call', 7: 'unimplemented', 10: 'trap', 11: 'first swap'}
rows = []
for i in range(1, runs + 1):
    err = (out / f'run{i}.err').read_text(errors='replace')
    code = int((out / f'run{i}.exit').read_text())
    stops = [l for l in err.splitlines() if l.startswith('[stop ')]
    first = stops[0] if stops else None
    rows.append({'run': i, 'exit_code': code, 'reached_entry': '[runtime] calling the title entry' in err,
                 'first_stop': first, 'stop_matches_exit': bool(first and first.startswith(f'[stop {code}]')),
                 'audio_line': next((l for l in err.splitlines() if l.startswith(AUDIO_LINE)), None)})
ok = all(r['reached_entry'] and r['exit_code'] in STOPS and r['stop_matches_exit'] and r['audio_line'] for r in rows) and \
    len({(r['exit_code'], r['first_stop']) for r in rows}) == 1
summary = {'runs': rows, 'passed': ok, 'kind': STOPS.get(rows[0]['exit_code'])}
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(f"run_attempt: {'PASS' if ok else 'FAIL'}, {runs} run(s), first stop: {rows[0]['first_stop']}")
sys.exit(0 if ok else 1)
PY
