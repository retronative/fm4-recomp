#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Bounded launcher (M2): build, then run the native binary <runs> times and record each run's stop.
#
#   scripts/run_attempt.sh <evidence dir> <runs> <deadline seconds>
#
# Rules (run safety): scripts/preflight.sh must say free; the run chains on a
# successful scripts/build.sh and prints the binary's SHA-256; each run has an isolated HOME under
# game/, its own process group and a deadline (SIGTERM, then SIGKILL of the group); runs are
# sequential. M2 links no audio (no SDL): the dummy-driver check does not apply and is recorded so.
# A run passes when it logged "[runtime] calling the title entry" and ended in a strict stop whose
# exit code matches its first "[stop N]" line (codes in runtime/stops.h: 3 import, 4 residual,
# 5 guest fault, 6 indirect call, 7 unimplemented, 10 trap). Every run must end identically.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -eq 3 ] || { echo "usage: scripts/run_attempt.sh <evidence dir> <runs> <deadline seconds>" >&2; exit 2; }
OUT="$1"; RUNS="$2"; DEADLINE="$3"
[[ "$RUNS" =~ ^[1-9][0-9]*$ && "$DEADLINE" =~ ^[1-9][0-9]*$ ]] || { echo "runs and deadline must be positive integers" >&2; exit 2; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
"$ROOT/scripts/preflight.sh"
"$ROOT/scripts/build.sh" "$OUT/build"
BIN="$ROOT/build/native/runtime/fm4recomp"
IMAGE="$ROOT/game/survey/image.bin"
XEX="$ROOT/game/files/default.xex"
DISC="$ROOT/game/files"
shasum -a 256 "$BIN" "$IMAGE" | sed "s|$ROOT/||" | tee "$OUT/binary.sha256"
echo "audio: none linked in M2 (no SDL); the dummy-driver check does not apply" | tee "$OUT/audio.txt"
export HOME="$ROOT/game/home"
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
    "$DEADLINE" "$BIN" --image "$IMAGE" --xex "$XEX" --disc "$DISC" --deadline "$DEADLINE" > "$OUT/run$i.out" 2> "$OUT/run$i.err" || status=$?
  echo "$status" > "$OUT/run$i.exit"
  echo "run $i: exit $status, $(grep -m1 '^\[stop ' "$OUT/run$i.err" || echo 'no stop line')"
done
/opt/homebrew/bin/python3 -B -I - "$OUT" "$RUNS" <<'PY'
import json, sys
from pathlib import Path
out, runs = Path(sys.argv[1]), int(sys.argv[2])
STOPS = {3: 'import', 4: 'residual', 5: 'guest fault', 6: 'indirect call', 7: 'unimplemented', 10: 'trap'}
rows = []
for i in range(1, runs + 1):
    err = (out / f'run{i}.err').read_text(errors='replace')
    code = int((out / f'run{i}.exit').read_text())
    stops = [l for l in err.splitlines() if l.startswith('[stop ')]
    first = stops[0] if stops else None
    rows.append({'run': i, 'exit_code': code, 'reached_entry': '[runtime] calling the title entry' in err,
                 'first_stop': first, 'stop_matches_exit': bool(first and first.startswith(f'[stop {code}]'))})
ok = all(r['reached_entry'] and r['exit_code'] in STOPS and r['stop_matches_exit'] for r in rows) and \
    len({(r['exit_code'], r['first_stop']) for r in rows}) == 1
summary = {'runs': rows, 'passed': ok, 'kind': STOPS.get(rows[0]['exit_code'])}
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(f"run_attempt: {'PASS' if ok else 'FAIL'}, {runs} run(s), first stop: {rows[0]['first_stop']}")
sys.exit(0 if ok else 1)
PY
