#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp alpha: plays the game in a window, with sound, a gamepad or the keyboard. Run ./setup.sh first (same FM4_RECOMP_HOME).
#   ./play.sh [minutes]       (the session ends after this many minutes, default 120; closing the window ends it too)
# GPU safeguards, the same as the developers' scripts/gpu_run.sh: one session at a time; the Vulkan validation layer is always on
# (window mode requires it) and is proven live first by a deliberate, query-only misuse it must report; the session runs in its
# own process group under a hard deadline; afterwards the system log and the crash reports are searched for a GPU hang or restart,
# and on any evidence ./GPU_STOP is written: every later session refuses to start until you read it and delete it yourself.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MINUTES="${1:-120}"
[[ "$MINUTES" =~ ^[1-9][0-9]*$ ]] || { echo "usage: ./play.sh [minutes]" >&2; exit 2; }
DEADLINE=$((MINUTES * 60))
ROOT="${FM4_RECOMP_HOME:-$HERE}"  # the data, logs and GPU_STOP (FM4 Recomp.app: ~/Library/Application Support/FM4 Recomp)
DATA="$ROOT/data"
STOP_FLAG="$ROOT/GPU_STOP"
LOCK="$ROOT/.play.lock"
LOGS="$ROOT/logs/$(date '+%Y-%m-%d_%H%M%S')"
for f in "$DATA/survey/image.bin" "$DATA/files/default.xex" "$DATA/survey/disc1_gdfx.tsv"; do
  [ -e "$f" ] || { echo "play: $f is missing: run ./setup.sh with your own Disc 1 image first" >&2; exit 1; }
done
if [ -e "$STOP_FLAG" ]; then
  echo "play: refused: $STOP_FLAG exists (an earlier session showed GPU hang evidence). Read it, then delete it yourself:" >&2
  cat "$STOP_FLAG" >&2; exit 3
fi
# One game on this Mac at a time, whatever data folder it uses (the lock below is per folder).
if other="$(pgrep -x fm4recomp)"; then echo "play: refused: FM4 Recomp is already running (pid $(echo $other | tr '\n' ' '))" >&2; exit 3; fi
if ! mkdir "$LOCK" 2>/dev/null; then
  holder="$(cat "$LOCK/pid" 2>/dev/null || true)"
  if [ -n "$holder" ] && kill -0 "$holder" 2>/dev/null; then echo "play: another session is running (pid $holder)" >&2; exit 3; fi
  rm -f "$LOCK/pid"; rmdir "$LOCK"; mkdir "$LOCK"
fi
echo $$ > "$LOCK/pid"
trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true' EXIT
mkdir -p "$LOGS/captures" "$DATA/user-data" "$DATA/home"

# The kit's own Vulkan: its loader, the MoltenVK driver and the validation layer, by their manifests (scripts/release/bundle_kit.sh);
# nothing from Homebrew or the developer tools. VK_LOADER_DEBUG=layer logs the layer's insertion (the session's check below).
LAYERS="$HERE/vulkan/explicit_layer.d"
ICD="$HERE/vulkan/icd.d/MoltenVK_icd.json"
LOADER="$HERE/lib/libvulkan.1.dylib"
for f in "$LAYERS/VkLayer_khronos_validation.json" "$ICD" "$LOADER" "$HERE/lib/libVkLayer_khronos_validation.dylib" "$HERE/lib/libMoltenVK.dylib"; do
  [ -e "$f" ] || { echo "play: $f is missing: this copy of FM4 Recomp is incomplete; download it again" >&2; exit 1; }
done
mkdir -p "$LOGS"
# VK_IMPLICIT_LAYER_PATH: the kit's empty folder, so no implicit layer installed elsewhere on the Mac loads into the game.
export VK_IMPLICIT_LAYER_PATH="$HERE/vulkan/implicit_layer.d" VK_LAYER_PATH="$LAYERS" VK_DRIVER_FILES="$ICD" VK_LOADER_DEBUG=layer MTL_DEBUG_LAYER=1 SDL_VULKAN_LIBRARY="$LOADER"
perl -e 'alarm 30; exec { $ARGV[0] } @ARGV' "$HERE/bin/fm4_validation_control" > "$LOGS/validation_control.log" 2>&1 || true
grep -q 'VUID-VkPhysicalDeviceProperties2-sType-sType' "$LOGS/validation_control.log" ||
  { echo "play: the validation layer did not report its negative control: not starting (see $LOGS/validation_control.log)" >&2; exit 1; }

# The alpha's settings (the developers' tested stack at this build): diagnostics off.
export FM4_DEFER_FENCES=1 FM4_GPU_TIMESTAMPS=1 FM4_RING_MB=256 FM4_WRITE_WATCH=1 FM4_RESOLVE_RESIDENT=1 FM4_PASS_MERGE=1 FM4_VERTEX_REUSE=1 \
  FM4_RESOLVE_RESIDENT_MIPS=1 FM4_INDEX_REUSE=1 FM4_RESOLVE_GPU_WORDS=1 FM4_SUBMIT_SPLIT=128 FM4_BULK_CONSTANTS=1 FM4_FAST_DETILE=1 FM4_DEFER_EVENTS=1 \
  FM4_RESOLVE_RESIDENT_K88=1 FM4_SHADER_NO_PRECISE=1 FM4_TRANSFER_STENCIL_EXPORT=2 FM4_RESOLVE_HOST_WRITE=1 FM4_ASYNC_SUBMIT=1 FM4_WAIT_POLL_US=2000 \
  FM4_OVERWRITE_CLAIM=1 FM4_FETCH_UNCHECKED=2 FM4_RESOLVE_RESIDENT_DEPTH=1 FM4_SHADER_MEMO=1 FM4_RESOLVE_GPU_WORDS16=1 FM4_RESOLVE_RESIDENT_1616=1 \
  FM4_SPLIT_AT_PASS_END=1 FM4_CLEAR_DRAWS=1 FM4_CLEAR_VALUE_OWNERSHIP=1 FM4_RESOLVE_DEFER=1 FM4_INPUT_LIVE=1 \
  FM4_SKIPPABLE_WELCOME_VIDEO=1  # A skips the long welcome video before the first race (not skippable in retail FM4)
USER_HOME="$HOME"
export HOME="$DATA/home"
echo "play: starting (session limit $MINUTES min; logs in $LOGS)"
since=$(date +%s); touch "$LOGS/start"
status=0
perl -e '
  my ($deadline, @cmd) = @ARGV;
  my $pid = fork; die "fork: $!" unless defined $pid;
  if (!$pid) { setpgrp(0, 0); exec { $cmd[0] } @cmd; die "exec: $!\n"; }
  $SIG{ALRM} = sub { kill "TERM", -$pid; sleep 2; kill "KILL", -$pid; waitpid $pid, 0; print STDERR "[play] session limit: stopped\n"; exit 124 };
  alarm $deadline + 30; waitpid $pid, 0; alarm 0;
  my $s = $?; exit($s & 127 ? 128 + ($s & 127) : $s >> 8);' \
  "$DEADLINE" "$HERE/bin/fm4recomp" --image "$DATA/survey/image.bin" --xex "$DATA/files/default.xex" --disc "$DATA/files" \
  --disc-map "$DATA/survey/disc1_gdfx.tsv" --modules "$DATA/modules" --gpu-stop "$STOP_FLAG" --user-data "$DATA/user-data" --audio audible \
  --renderer window --gpu-capture "$LOGS/captures" --vk-validation on --shader-classes branch,predicate,loop,dynloop \
  --stop-at-swap 100000000 --deadline "$DEADLINE" > "$LOGS/game.out" 2> "$LOGS/game.log" || status=$?
echo "play: the game ended (exit $status): $(grep -m1 '^\[stop ' "$LOGS/game.log" || echo 'no stop line')"
sleep 2
evidence=""
while IFS= read -r -d '' file; do evidence+="gpuEvent report: $file"$'\n'; done < <(find "$USER_HOME/Library/Logs/DiagnosticReports" /Library/Logs/DiagnosticReports -maxdepth 1 -name 'gpuEvent-*.ips' \
  -newer "$LOGS/start" -print0 2>/dev/null)
/usr/bin/log show --style compact --start "$(date -r "$since" '+%Y-%m-%d %H:%M:%S')" \
  --predicate '(process == "fm4recomp" OR eventMessage CONTAINS "GPU Hang" OR eventMessage CONTAINS "IOGPU" OR eventMessage CONTAINS "GPU Restart" OR subsystem CONTAINS "Metal") AND process != "log"' \
  > "$LOGS/syslog.txt" 2>/dev/null || true
evidence+="$(grep -Ev '^[0-9: .-]+ +[A-Za-z]+ +log\[[0-9]+:' "$LOGS/syslog.txt" | grep -E 'GPU Hang|kIOGPUCommandBufferCallbackError|GPU Restart|gpu restart|IOGPUFamily.*(timeout|timed out|hang|lockup|panic)|GPU.*(hang|lockup) detected' | head -20 || true)"
if [ -n "$evidence" ]; then
  { echo "GPU_STOP written $(date '+%Y-%m-%d %H:%M:%S') by play.sh: GPU hang evidence after a session; delete this file yourself to play again."; echo "$evidence"; } > "$STOP_FLAG"
  echo "play: GPU HANG EVIDENCE: wrote $STOP_FLAG (please send it and $LOGS to the developers)" >&2; exit 3
fi
grep -q 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$LOGS/game.log" ||
  echo "play: WARNING: the game did not load the validation layer (a GPU safeguard); please send $LOGS/game.log to the developers" >&2
if grep -E 'Validation (Error|Warning)|\[vulkan validation\]' "$LOGS/game.log" | head -5; then
  echo "play: the validation layer reported problems (above); please send $LOGS/game.log to the developers" >&2
fi
echo "play: session over, no GPU hang evidence. Log: $LOGS/game.log"
