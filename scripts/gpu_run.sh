#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# The watchdog launcher for every run that may touch the GPU (run safety; M4 before the
# first GPU run). After ForzaRecomp's gpu_run.sh (zsh), re-written in bash: a zsh
# `status` variable once disabled a watchdog silently.
#
#   scripts/gpu_run.sh <evidence dir> <runs> <deadline seconds> <stop at swap n> <audio: audible|mute> <renderer: headless|self-check|present|window>
#   scripts/gpu_run.sh --self-test        # checks the refusal and lock-up detection on planted files; runs nothing
#
# Before: refuses if research/GPU_STOP exists (only the owner clears it), runs scripts/preflight.sh (the machine must
# be free), builds, records the binary's SHA-256. Each run: its own process group, the deadline, then TERM and KILL.
# After each run: a new gpuEvent-*.ips report in the diagnostic reports, or a GPU restart / timeout message in the
# system log since the run started, is a lock-up: research/GPU_STOP is written with the evidence and the launcher
# fails. The runtime's own GPU entry points honour GPU_STOP too (M4 renderer work).
# A renderer other than headless (M4): the pinned Khronos validation layer (1.4.363) is inserted and the pinned
# MoltenVK ICD is the only driver; Metal API validation is on (MTL_DEBUG_LAYER=1). Before the runs the negative
# control (build/native/tests/fm4_validation_control: a wrong sType in a physical-device query, no device or
# resource) must make the layer report VUID-VkPhysicalDeviceProperties2-sType-sType, else nothing runs.
# After each run its log must show the layer inserted and no validation message; captures go to run<i>.captures/.
# GPU_RUN_VALIDATION_OFF="<approval note>" (approved single runs only, e.g. a native frame-time measurement): no
# layer, no Metal API validation, fm4recomp --vk-validation off; the approval text goes to validation.txt. Every other
# safeguard (GPU_STOP, lock, preflight, deadline, lock-up checks, the pinned MoltenVK ICD) is unchanged.
# window (unit 4b): present plus a visible window on the main thread (runtime/video/window_sdl.h). Everything above
# applies unchanged; in addition SDL must load the same Vulkan loader fm4recomp links (SDL_VULKAN_LIBRARY below; the
# runtime checks it and stops otherwise), each run's log must show its "[window] swapchain:" line, and exit 14 is the
# user closing the window (a named stop, not a failure of the launcher).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STOP_FLAG="$ROOT/research/GPU_STOP"
REPORT_DIRS=("$HOME/Library/Logs/DiagnosticReports" "/Library/Logs/DiagnosticReports")
# Lock-up lines. "GPU Hang Error ... kIOGPUCommandBufferCallbackErrorHang" is the line Metal logged in ForzaRecomp-recarbon's
# real hang (its syslog), which left no .ips report.
LOG_PATTERN='GPU Hang|kIOGPUCommandBufferCallbackError|GPU Restart|gpu restart|IOGPUFamily.*(timeout|timed out|hang|lockup|panic)|GPU.*(hang|lockup) detected'
# The log tool records its own invocation, predicate text included ("GPU Hang"), so its lines are excluded twice
# (predicate and lockup_lines): otherwise the check matches itself (this once
# wrote a false GPU_STOP).
LOG_PREDICATE='(process == "fm4recomp" OR eventMessage CONTAINS "GPU Hang" OR eventMessage CONTAINS "IOGPU" OR eventMessage CONTAINS "GPU Restart" OR subsystem CONTAINS "Metal") AND process != "log"'
LOCK="$ROOT/research/.gpu_run.lock"

# New lock-up evidence (reports newer than the marker file $1) in the report directories given after it; prints
# it, returns 0 if any. (BSD find has no epoch form for -newermt, so a marker file touched at the run's start.)
lockup_evidence() {
  local marker="$1"; shift
  local found=1 dir file
  for dir in "$@"; do
    [ -d "$dir" ] || continue
    while IFS= read -r -d '' file; do
      echo "gpuEvent report: $file"
      found=0
    done < <(find "$dir" -maxdepth 1 -name 'gpuEvent-*.ips' -newer "$marker" -print0 2>/dev/null)
  done
  return $found
}

# The system log since epoch $1 (GPU and Metal lines, and this binary's), saved to $2; prints its lock-up lines.
log_evidence() {
  local since="$1" saved="$2"
  /usr/bin/log show --style compact --start "$(date -r "$since" '+%Y-%m-%d %H:%M:%S')" --predicate "$LOG_PREDICATE" > "$saved" 2>/dev/null || true
  lockup_lines "$saved"
}

# Lock-up lines in a saved log, never the log tool's own lines (its invocation repeats the predicate).
lockup_lines() {
  grep -Ev '^[0-9: .-]+ +[A-Za-z]+ +log\[[0-9]+:' "$1" | grep -E "$LOG_PATTERN" | head -20 || true
}

# One GPU run on the machine at a time: a mkdir lock holding the owner's pid; a lock whose pid is gone is reclaimed.
take_lock() {
  if ! mkdir "$LOCK" 2>/dev/null; then
    local holder; holder="$(cat "$LOCK/pid" 2>/dev/null || true)"
    if [ -n "$holder" ] && kill -0 "$holder" 2>/dev/null; then
      echo "gpu_run: refused: another GPU run holds $LOCK (pid $holder)" >&2; exit 3
    fi
    rm -f "$LOCK/pid" && rmdir "$LOCK" && mkdir "$LOCK" || { echo "gpu_run: cannot take $LOCK" >&2; exit 3; }
  fi
  echo $$ > "$LOCK/pid"
  trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true' EXIT
}

# Refuses (exit 3) if the stop flag $1 exists.
refuse_if_stopped() {
  if [ -e "$1" ]; then
    echo "gpu_run: refused: $1 exists (an earlier lock-up); only the owner clears it:" >&2
    cat "$1" >&2
    exit 3
  fi
}

trip() {
  { echo "GPU_STOP written $(date '+%Y-%m-%d %H:%M:%S') by scripts/gpu_run.sh; only the owner clears it."; echo "$1"; } > "$STOP_FLAG"
  echo "gpu_run: LOCK-UP EVIDENCE, wrote $STOP_FLAG:" >&2
  echo "$1" >&2
  exit 3
}

if [ "${1:-}" = "--self-test" ]; then
  mkdir -p "$ROOT/build"
  work="$(mktemp -d "$ROOT/build/gpu_run_selftest.XXXXXX")"
  touch -t 202601010000 "$work/marker"
  if lockup_evidence "$work/marker" "$work" >/dev/null; then echo "self-test FAIL: evidence in an empty directory" >&2; exit 1; fi
  touch "$work/gpuEvent-2026-11-06-000000.ips"
  if ! lockup_evidence "$work/marker" "$work" | grep -q gpuEvent-2026; then echo "self-test FAIL: a new gpuEvent report not detected" >&2; exit 1; fi
  touch -t 202001010000 "$work/gpuEvent-old.ips"
  if lockup_evidence "$work/marker" "$work" | grep -q gpuEvent-old; then echo "self-test FAIL: an old report counted" >&2; exit 1; fi
  hang='2026-11-05 10:29:20.283 E  forzarecomp[98678:17bada] (Metal) Execution of the command buffer was aborted due to an error during execution. Caused GPU Hang Error (00000003:kIOGPUCommandBufferCallbackErrorHang)'
  if echo "$hang" | grep -Eq "$LOG_PATTERN" && echo "kernel: IOGPUFamily: GPU Restart" | grep -Eq "$LOG_PATTERN" &&
     ! echo "fm4recomp: [runtime] calling the title entry" | grep -Eq "$LOG_PATTERN"; then :; else
    echo "self-test FAIL: the log pattern (the real Metal hang line, a GPU restart, an ordinary line)" >&2; exit 1; fi
  # The earlier false positive: the log tool's own invocation line (it repeats the predicate) must not count;
  # the real hang line in the same log must.
  printf '%s\n' "2026-11-06 13:53:20.064 Df log[33771:7df138] [com.apple.log:] log run noninteractively, args: '/usr/bin/log' 'show' '--predicate' 'eventMessage CONTAINS \"GPU Hang\"'" "$hang" > "$work/self.log"
  { [ "$(lockup_lines "$work/self.log" | wc -l | tr -d ' ')" = 1 ] && lockup_lines "$work/self.log" | grep -q kIOGPUCommandBufferCallbackErrorHang; } ||
    { echo "self-test FAIL: the log tool's own line counted, or the hang line missed" >&2; exit 1; }
  # The real pipeline (log show + predicate + filter) on the real system log: a clearly marked hang line written with
  # logger must be found. The real lock is held meanwhile, so no GPU run can see this line in its window.
  take_lock
  since=$(date +%s)
  logger -t fm4-recomp-gpu-run-selftest "fm4-recomp gpu_run self-test (not a real hang): Caused GPU Hang Error (00000003:kIOGPUCommandBufferCallbackErrorHang)"
  found=""
  for attempt in 1 2 3 4 5; do
    sleep 1
    found="$(log_evidence "$since" "$work/real.log" | grep 'gpu_run self-test' || true)"
    [ -n "$found" ] && break
  done
  [ -n "$found" ] || { echo "self-test FAIL: the real system log pipeline did not find the marked hang line" >&2; exit 1; }
  rm -f "$LOCK/pid"; rmdir "$LOCK"; trap - EXIT
  LOCK="$work/lock"
  ( take_lock; [ -f "$LOCK/pid" ] ) || { echo "self-test FAIL: lock not taken" >&2; exit 1; }
  [ ! -d "$LOCK" ] || { echo "self-test FAIL: lock not released" >&2; exit 1; }
  mkdir "$LOCK" && echo 999999 > "$LOCK/pid"
  ( take_lock ) || { echo "self-test FAIL: a stale lock not reclaimed" >&2; exit 1; }
  mkdir "$LOCK" && echo $$ > "$LOCK/pid"
  if ( take_lock ) 2>/dev/null; then echo "self-test FAIL: a live lock not refused" >&2; exit 1; fi
  rm -f "$LOCK/pid" && rmdir "$LOCK"
  STOP_FLAG="$work/GPU_STOP"
  if ( trip "planted evidence" ) 2>/dev/null; then echo "self-test FAIL: trip did not fail" >&2; exit 1; fi
  grep -q "planted evidence" "$STOP_FLAG" || { echo "self-test FAIL: GPU_STOP not written" >&2; exit 1; }
  if ( refuse_if_stopped "$STOP_FLAG" ) 2>/dev/null; then echo "self-test FAIL: a stop flag did not refuse" >&2; exit 1; fi
  ( refuse_if_stopped "$work/absent" ) || { echo "self-test FAIL: refused without a stop flag" >&2; exit 1; }
  rm -r "$work"  # the self-test's own temporary directory under build/
  echo "gpu_run self-test: PASS (reports: empty, new, old; log pattern: the real Metal hang line, a GPU restart, an ordinary line, the log tool's own line excluded; the real system log (a marked logger line found); GPU_STOP written; refusal with and without a flag; lock: taken, released, stale reclaimed, live refused)"
  exit 0
fi

[ $# -eq 6 ] || [ $# -eq 7 ] || { echo "usage: scripts/gpu_run.sh <evidence dir> <runs> <deadline seconds> <stop at swap n> <audio: audible|mute> <renderer: headless|self-check|present|window> [<shader classes: none|branch,predicate,loop,dynloop>] | --self-test" >&2; exit 2; }
# The shader-class allow-list (runtime/video/shader_class.h) is required by fm4recomp with a GPU renderer mode and has no
# default: a run that may create pipelines names the classes approved for it; "none" allows straight-line shaders only.
SHADER_CLASSES="${7:-}"
OUT="$1"; RUNS="$2"; DEADLINE="$3"; STOP_AT_SWAP="$4"; AUDIO="$5"; RENDERER="$6"
[[ "$AUDIO" == audible || "$AUDIO" == mute ]] || { echo "audio must be audible or mute" >&2; exit 2; }
[[ "$RENDERER" == headless || "$RENDERER" == self-check || "$RENDERER" == present || "$RENDERER" == window ]] || { echo "renderer must be headless, self-check, present or window" >&2; exit 2; }
if [ "$RENDERER" != headless ]; then
  [[ "$SHADER_CLASSES" =~ ^(none|(branch|predicate|loop|dynloop)(,(branch|predicate|loop|dynloop))*)$ ]] || { echo "a GPU renderer needs the shader classes: none, or a list of branch, predicate, loop, dynloop (fm4recomp --shader-classes)" >&2; exit 2; }
else
  [ -z "$SHADER_CLASSES" ] || { echo "shader classes apply to a GPU renderer only" >&2; exit 2; }
fi
[[ "$STOP_AT_SWAP" =~ ^[1-9][0-9]*$ ]] || { echo "the swap to stop at must be a positive integer" >&2; exit 2; }
[[ "$RUNS" =~ ^[1-9][0-9]*$ && "$DEADLINE" =~ ^[1-9][0-9]*$ ]] || { echo "runs and deadline must be positive integers" >&2; exit 2; }
refuse_if_stopped "$STOP_FLAG"
take_lock
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
"$ROOT/scripts/preflight.sh" --gpu
"$ROOT/scripts/build.sh" "$OUT/build"
BIN="$ROOT/build/native/runtime/fm4recomp"
shasum -a 256 "$BIN" "$ROOT/game/survey/image.bin" | sed "s|$ROOT/||" | tee "$OUT/binary.sha256"
echo "audio: $AUDIO (fm4recomp --audio $AUDIO; the run's \"[audio]\" line names the device or says none was opened)" | tee "$OUT/audio.txt"
GPU_ARGS=(--renderer "$RENDERER")
VALIDATION=on
if [ -n "${GPU_RUN_VALIDATION_OFF:-}" ]; then
  [ "$RENDERER" != headless ] || { echo "GPU_RUN_VALIDATION_OFF applies to a GPU renderer only" >&2; exit 2; }
  VALIDATION=off
fi
if [ "$RENDERER" != headless ] && [ "$VALIDATION" = off ]; then
  ICD=/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json
  [ -f "$ICD" ] || { echo "gpu_run: the MoltenVK ICD $ICD is missing" >&2; exit 1; }
  [ "$RENDERER" != window ] || { echo "gpu_run: GPU_RUN_VALIDATION_OFF is not supported with window" >&2; exit 2; }
  export VK_DRIVER_FILES="$ICD" VK_LOADER_DEBUG=layer
  shasum -a 256 "$ICD" | tee -a "$OUT/binary.sha256"
  echo "validation: OFF (no layer, no Metal API validation); approval: $GPU_RUN_VALIDATION_OFF" | tee "$OUT/validation.txt"
elif [ "$RENDERER" != headless ]; then
  LAYERS=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d
  ICD=/opt/homebrew/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json
  grep -q '"api_version": "1.4.363"' "$LAYERS/VkLayer_khronos_validation.json" || { echo "gpu_run: the validation layer is not the pinned 1.4.363" >&2; exit 1; }
  [ -f "$ICD" ] || { echo "gpu_run: the MoltenVK ICD $ICD is missing" >&2; exit 1; }
  # The layer manifest names its library without a path ("libVkLayer_khronos_validation.dylib"), which the loader
  # resolves through dyld's search path; DYLD_* variables do not survive the /usr/bin/perl launcher below (SIP strips
  # them). So the run uses a copy of the manifest whose library_path is absolute.
  mkdir -p "$OUT/vulkan"
  sed 's|"library_path": "libVkLayer_khronos_validation.dylib"|"library_path": "/opt/homebrew/opt/vulkan-validationlayers/lib/libVkLayer_khronos_validation.dylib"|' \
    "$LAYERS/VkLayer_khronos_validation.json" > "$OUT/vulkan/VkLayer_khronos_validation.json"
  grep -q '"library_path": "/opt/homebrew/opt/vulkan-validationlayers/lib/libVkLayer_khronos_validation.dylib"' "$OUT/vulkan/VkLayer_khronos_validation.json" ||
    { echo "gpu_run: the layer manifest's library_path was not rewritten" >&2; exit 1; }
  # The ICD's library_path (../../../lib/libMoltenVK.dylib) is relative to its manifest: the loader resolves it.
  export VK_LAYER_PATH="$OUT/vulkan" VK_DRIVER_FILES="$ICD" VK_LOADER_DEBUG=layer MTL_DEBUG_LAYER=1
  shasum -a 256 "$ICD" "$LAYERS/VkLayer_khronos_validation.json" "$OUT/vulkan/VkLayer_khronos_validation.json" \
    /opt/homebrew/opt/vulkan-validationlayers/lib/libVkLayer_khronos_validation.dylib | tee -a "$OUT/binary.sha256"
  if [ "$RENDERER" = window ]; then
    # SDL opens a Vulkan loader of its own for the window (SDL_Vulkan_LoadLibrary): it must be the one fm4recomp links
    # (otool -L), named absolutely (a bare name would go through dyld's search path). The runtime compares the two
    # vkGetInstanceProcAddr and stops (9) if they differ.
    LOADER=/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib
    [ -f "$LOADER" ] || { echo "gpu_run: the Vulkan loader $LOADER is missing" >&2; exit 1; }
    otool -L "$BIN" | grep -qF "$LOADER" || { echo "gpu_run: fm4recomp does not link $LOADER" >&2; exit 1; }
    export SDL_VULKAN_LIBRARY="$LOADER"
    shasum -a 256 "$LOADER" | tee -a "$OUT/binary.sha256"
  fi
  # Negative control first: a deliberate, query-only misuse the layer must report (no device, no GPU work).
  CONTROL="$ROOT/build/native/tests/fm4_validation_control"
  perl -e 'alarm 30; exec @ARGV' "$CONTROL" > "$OUT/validation_control.log" 2>&1 || true
  grep -q 'VUID-VkPhysicalDeviceProperties2-sType-sType' "$OUT/validation_control.log" ||
    { echo "gpu_run: negative control: the layer did not report the deliberate misuse; nothing runs" >&2; exit 1; }
  echo "validation: negative control reported (VUID-VkPhysicalDeviceProperties2-sType-sType)" | tee "$OUT/validation.txt"
fi
export HOME="$ROOT/game/home"
mkdir -p "$ROOT/game/user-data"  # saved content (kernel/content.h), isolated from the read-only disc
mkdir -p "$HOME"
for i in $(seq 1 "$RUNS"); do
  since=$(date +%s)
  touch "$OUT/run$i.start"
  RUN_GPU_ARGS=("${GPU_ARGS[@]}")
  if [ "$RENDERER" != headless ]; then
    mkdir "$OUT/run$i.captures"  # fails if it exists: evidence is never overwritten
    RUN_GPU_ARGS+=(--gpu-capture "$OUT/run$i.captures" --vk-validation "$VALIDATION" --shader-classes "$SHADER_CLASSES")
  fi
  status=0
  perl -e '
    my ($deadline, @cmd) = @ARGV;
    my $pid = fork; die "fork: $!" unless defined $pid;
    if (!$pid) { setpgrp(0, 0); exec @cmd; die "exec: $!\n"; }
    $SIG{ALRM} = sub { kill "TERM", -$pid; sleep 2; kill "KILL", -$pid; waitpid $pid, 0; print STDERR "[launcher] deadline: process group killed\n"; exit 124 };
    alarm $deadline + 30; waitpid $pid, 0; alarm 0;
    my $s = $?; exit($s & 127 ? 128 + ($s & 127) : $s >> 8);' \
    "$DEADLINE" "$BIN" --image "$ROOT/game/survey/image.bin" --xex "$ROOT/game/files/default.xex" --disc "$ROOT/game/files" --disc-map "$ROOT/game/survey/disc1_gdfx.tsv" \
    --modules "$ROOT/game/modules" --gpu-stop "$STOP_FLAG" --user-data "$ROOT/game/user-data" --audio "$AUDIO" "${RUN_GPU_ARGS[@]}" --stop-at-swap "$STOP_AT_SWAP" --deadline "$DEADLINE" > "$OUT/run$i.out" 2> "$OUT/run$i.err" || status=$?
  echo "$status" > "$OUT/run$i.exit"
  echo "run $i: exit $status, $(grep -m1 '^\[stop ' "$OUT/run$i.err" || echo 'no stop line')"
  sleep 2  # let reports and log lines land
  evidence="$(lockup_evidence "$OUT/run$i.start" "${REPORT_DIRS[@]}" || true)$(log_evidence "$since" "$OUT/run$i.syslog.txt")"
  [ -z "$evidence" ] || trip "run $i ($OUT): $evidence"
  # The log must name the audio mode given (audible: driver and device; mute: no device opened).
  if [ "$AUDIO" = mute ]; then audio_line='[audio] mute: no audio device opened'; else audio_line='[audio] audible: '; fi
  grep -F -m1 "$audio_line" "$OUT/run$i.err" >> "$OUT/audio.txt" || { echo "run $i: no \"$audio_line\" line in its log" >&2; exit 1; }
  if [ "$RENDERER" != headless ] && [ "$VALIDATION" = off ]; then
    if grep -q 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$OUT/run$i.err"; then echo "run $i: a validation layer was inserted in a validation-off run" >&2; exit 1; fi
    grep -m1 'validation OFF' "$OUT/run$i.err" | tee -a "$OUT/validation.txt" || { echo "run $i: no \"validation OFF\" line in its log" >&2; exit 1; }
  elif [ "$RENDERER" != headless ]; then
    grep -q 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$OUT/run$i.err" || { echo "run $i: the validation layer was not inserted" >&2; exit 1; }
    if grep -E 'Validation (Error|Warning)|Validation Performance Warning|\[vulkan validation\]|failed to load|failed assertion' "$OUT/run$i.err" | head -20; then
      echo "run $i: validation messages (above)" >&2; exit 1
    fi
    echo "run $i: layer inserted, no validation message" | tee -a "$OUT/validation.txt"
    if [ "$RENDERER" = self-check ] && [ "$status" != 12 ]; then echo "run $i: the self-check did not pass (exit $status, expected 12)" >&2; exit 1; fi
    if [ "$RENDERER" = window ]; then
      grep -m1 '^\[window\] swapchain: ' "$OUT/run$i.err" | tee -a "$OUT/validation.txt" || { echo "run $i: no \"[window] swapchain:\" line in its log" >&2; exit 1; }
      if [ "$status" = 14 ]; then echo "run $i: the window was closed by the user (stop 14)"; fi
    fi
  fi
done
echo "gpu_run: $RUNS run(s), no lock-up evidence"
# The runs' performance baselines beside their logs (scripts/perf_baseline.py: one per bounded
# run). Read-only on the logs; a failure is reported, never the run's result.
/opt/homebrew/bin/python3 -B -I "$ROOT/scripts/perf_baseline.py" "$OUT" > "$OUT/perf_baseline.txt" 2>&1 || echo "gpu_run: perf baseline failed (see $OUT/perf_baseline.txt)" >&2
