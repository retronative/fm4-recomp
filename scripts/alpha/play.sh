#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp alpha: plays the game in a window, with sound, a gamepad or the keyboard. Run ./setup.sh first (same FM4_RECOMP_HOME).
#   ./play.sh [minutes] [--debug]   (the session ends after this many minutes, default 120; closing the window ends it too)
#   ./play.sh --warm-up [minutes]    (default 90) the shader warm-up: the game, hidden and muted, plays the kit's scripted route (title,
#   Free Play, ten Quick Races; warmup/route.txt) on a throwaway copy of the kit's prepared profile (warmup/profile: past the welcome
#   race), with the player's graphics settings, into the player's shader cache, so those shaders are compiled before the player meets
#   them. The player's saves are never opened. It prints "progress <percent> <stage>" lines; it ends at the route's end (exit 0), or
#   when stopped (TERM: the shaders compiled so far are kept).
# Normal play: no Vulkan validation layer and no Metal API validation, which cost
# about two thirds of the renderer's CPU time. --debug (or FM4_RECOMP_DEBUG=1) restores full validation: the pinned layer, proven
# live first by a deliberate, query-only misuse it must report, Metal API validation, and a stop at the first validation message.
# The GPU safeguards stay in both modes: one session at a time; the game's own fence timeouts and GPU_STOP checks; the session runs
# in its own process group under a hard deadline; afterwards the system log and the crash reports are searched for a GPU hang or
# restart, and on any evidence ./GPU_STOP is written: every later session refuses to start until you read it and delete it yourself.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MINUTES=""; DEBUG="${FM4_RECOMP_DEBUG:-0}"; WARMUP=0
for arg in "$@"; do
  case "$arg" in
    --debug) DEBUG=1 ;;
    --warm-up) WARMUP=1 ;;
    *) [[ "$arg" =~ ^[1-9][0-9]*$ ]] || { echo "usage: ./play.sh [minutes] [--debug] | ./play.sh --warm-up [minutes]" >&2; exit 2; }; MINUTES="$arg" ;;
  esac
done
if [ "$WARMUP" = 1 ]; then MINUTES="${MINUTES:-90}"; DEBUG=0; else MINUTES="${MINUTES:-120}"; fi
DEADLINE=$((MINUTES * 60))
ROOT="${FM4_RECOMP_HOME:-$HERE}"  # locations.conf, logs and GPU_STOP (FM4 Recomp.app: ~/Library/Application Support/FM4 Recomp)
STOP_FLAG="$ROOT/GPU_STOP"
LOCK="$ROOT/.play.lock"
LOGS="$ROOT/logs/$(date '+%Y-%m-%d_%H%M%S')$([ "$WARMUP" = 1 ] && echo _warm-up || true)"
# The game data, user data and (once set up) Disc 2 where setup.sh recorded them (locations.conf; an external drive must be connected).
source "$HERE/locations.sh"
loc_game_args "$ROOT" || { echo "play: not starting" >&2; exit 1; }
DATA="$LOC_DATA_ROOT"
# The game's in-game Disc 2 setup (the launcher's setup panel, bin/fm4recomp-setup) runs this kit's setup.sh --disc2 against the
# same locations.conf; the game's KEYBOARD CONTROLS (Pause menu) writes $FM4_RECOMP_HOME/keybindings.conf.
export FM4_RECOMP_HOME="$ROOT" FM4_RECOMP_KIT="$HERE"
if [ -e "$STOP_FLAG" ]; then
  echo "play: refused: $STOP_FLAG exists (an earlier session showed GPU hang evidence). Read it, then delete it yourself:" >&2
  cat "$STOP_FLAG" >&2; exit 3
fi
# One game on this Mac at a time, whatever data folder it uses (the lock below is per folder).
if other="$(pgrep -x 'fm4recomp|fm4_static_recomp')"; then echo "play: refused: FM4 Recomp is already running (pid $(echo $other | tr '\n' ' '))" >&2; exit 3; fi
if ! mkdir "$LOCK" 2>/dev/null; then
  holder="$(cat "$LOCK/pid" 2>/dev/null || true)"
  if [ -n "$holder" ] && kill -0 "$holder" 2>/dev/null; then echo "play: another session is running (pid $holder)" >&2; exit 3; fi
  rm -f "$LOCK/pid"; rmdir "$LOCK"; mkdir "$LOCK"
fi
echo $$ > "$LOCK/pid"
trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true' EXIT
mkdir -p "$LOGS/captures" "$DATA/home"
if [ "$WARMUP" = 1 ]; then
  # The warm-up's own user data: a fresh copy of the prepared profile each time, removed afterwards; its keyboard bindings and Disc 2
  # setup are not the game's concern here (the route uses the scripted pad, and no Disc 2 car).
  ROUTE="$HERE/warmup/route.txt"
  for f in "$ROUTE" "$HERE/warmup/profile/content"; do
    [ -e "$f" ] || { echo "play: $f is missing: this copy of FM4 Recomp is incomplete; download it again" >&2; exit 1; }
  done
  WARM_USER="$DATA/warm-up/user-data"
  rm -rf "$DATA/warm-up"
  mkdir -p "$DATA/warm-up"
  cp -R "$HERE/warmup/profile" "$WARM_USER"
  trap 'rm -f "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; rm -rf "$DATA/warm-up"' EXIT
  args=()
  for ((i = 0; i < ${#LOC_GAME_ARGS[@]}; i++)); do
    case "${LOC_GAME_ARGS[$i]}" in
      --user-data) args+=(--user-data "$WARM_USER"); i=$((i + 1)) ;;
      --keybindings) i=$((i + 1)) ;;
      *) args+=("${LOC_GAME_ARGS[$i]}") ;;
    esac
  done
  LOC_GAME_ARGS=("${args[@]}")
else
# Saved games kept by FM4 Recomp 0.3.x (its own layout in the same user-data folder) replace this layout's older copies, once
# (migrate-from-0.3.swift). On any surprise nothing changes and the game does not start: the stale copy is never played.
if ! saves="$(/bin/bash "$HERE/migrate-from-0.3.sh" "$LOC_USER_DATA_ROOT" 2>&1)"; then
  printf '%s\n' "$saves" >&2
  echo "play: not starting: your 0.3 saved games could not be brought over: $(printf '%s\n' "$saves" | tail -1 | sed 's/^saves: //')" >&2
  exit 1
fi
[ -z "$saves" ] || printf '%s\n' "$saves"
fi

# The kit's own Vulkan: its loader and the MoltenVK driver, by their manifests (scripts/release/bundle_kit.sh); nothing from Homebrew or
# the developer tools. VK_IMPLICIT_LAYER_PATH: the kit's empty folder, so no implicit layer installed elsewhere loads into the game.
ICD="$HERE/vulkan/icd.d/MoltenVK_icd.json"
LOADER="$HERE/lib/libvulkan.1.dylib"
for f in "$ICD" "$LOADER" "$HERE/lib/libMoltenVK.dylib"; do
  [ -e "$f" ] || { echo "play: $f is missing: this copy of FM4 Recomp is incomplete; download it again" >&2; exit 1; }
done
mkdir -p "$LOGS"
unset VK_INSTANCE_LAYERS VK_LAYER_PATH VK_ADD_LAYER_PATH MTL_DEBUG_LAYER
export VK_IMPLICIT_LAYER_PATH="$HERE/vulkan/implicit_layer.d" VK_DRIVER_FILES="$ICD" VK_LOADER_DEBUG=layer SDL_VULKAN_LIBRARY="$LOADER"
VALIDATION=off
if [ "$DEBUG" = 1 ]; then
  LAYERS="$HERE/vulkan/explicit_layer.d"
  for f in "$LAYERS/VkLayer_khronos_validation.json" "$HERE/lib/libVkLayer_khronos_validation.dylib"; do
    [ -e "$f" ] || { echo "play: $f is missing: this copy of FM4 Recomp is incomplete; download it again" >&2; exit 1; }
  done
  export VK_LAYER_PATH="$LAYERS" MTL_DEBUG_LAYER=1
  perl -e 'alarm 30; exec { $ARGV[0] } @ARGV' "$HERE/bin/fm4_validation_control" > "$LOGS/validation_control.log" 2>&1 || true
  grep -q 'VUID-VkPhysicalDeviceProperties2-sType-sType' "$LOGS/validation_control.log" ||
    { echo "play: the validation layer did not report its negative control: not starting (see $LOGS/validation_control.log)" >&2; exit 1; }
  VALIDATION=on
  echo "play: debug mode: Vulkan validation and Metal API validation on (slower)"
fi

# The alpha's settings (the developers' tested stack at this build): diagnostics off.
export FM4_DEFER_FENCES=1 FM4_GPU_TIMESTAMPS=1 FM4_RING_MB=256 FM4_WRITE_WATCH=1 FM4_RESOLVE_RESIDENT=1 FM4_PASS_MERGE=1 FM4_VERTEX_REUSE=1 \
  FM4_RESOLVE_RESIDENT_MIPS=1 FM4_INDEX_REUSE=1 FM4_RESOLVE_GPU_WORDS=1 FM4_SUBMIT_SPLIT=128 FM4_BULK_CONSTANTS=1 FM4_FAST_DETILE=1 FM4_DEFER_EVENTS=1 \
  FM4_RESOLVE_RESIDENT_K88=1 FM4_SHADER_NO_PRECISE=1 FM4_TRANSFER_STENCIL_EXPORT=2 FM4_RESOLVE_HOST_WRITE=1 FM4_ASYNC_SUBMIT=1 FM4_WAIT_POLL_US=2000 \
  FM4_OVERWRITE_CLAIM=1 FM4_FETCH_UNCHECKED=2 FM4_RESOLVE_RESIDENT_DEPTH=1 FM4_SHADER_MEMO=1 FM4_RESOLVE_GPU_WORDS16=1 FM4_RESOLVE_RESIDENT_1616=1 \
  FM4_SPLIT_AT_PASS_END=1 FM4_CLEAR_DRAWS=1 FM4_CLEAR_VALUE_OWNERSHIP=1 FM4_RESOLVE_DEFER=1 FM4_INPUT_LIVE=1 \
  FM4_SKIPPABLE_WELCOME_VIDEO=1 FM4_PUSH_DESCRIPTORS=1 FM4_PAUSE_KEYBOARD_CONTROLS=1 FM4_LOAD_WAITS_RELEASES=1
# A skips the long welcome video before the first race (not skippable in retail FM4); the Pause menu has a KEYBOARD CONTROLS row
# (config/title_hooks.toml) that opens the KEYBOARD CONTROLS submenu, an option screen of the game's own (runtime/game/).
# FM4_LOAD_WAITS_RELEASES: a track-heap allocation that fails while releases are queued waits for them (the near road missing
# after some race starts; runtime/game/release_wait.cpp).
# Stack v2: shared guest memory read by the
# GPU, GPU present, the vertex cache, submit-when-idle at 8 draws (32 before: fewer long frames at QHD,
# research/m9/perf/RESULTS.md), bulk register runs, async retire. GPU texture untile
# (FM4_TEXTURE_GPU_UNTILE) is off: with it, Free Play -> Quick Race (Bernese Alps) drew the road and grass untextured.
# Each is a default: a value already in the environment wins (0 turns a setting off), so a defect can be bisected setting by setting.
export FM4_VARIANT_MEMO="${FM4_VARIANT_MEMO:-1}" FM4_GPU_PRESENT="${FM4_GPU_PRESENT:-1}" FM4_TEXTURE_PREPARE_ONCE="${FM4_TEXTURE_PREPARE_ONCE:-1}" \
  FM4_GUARD_HOT="${FM4_GUARD_HOT:-1}" FM4_GUEST_MEMORY_GPU="${FM4_GUEST_MEMORY_GPU:-1}" FM4_RESOLVE_RESIDENT_COVER="${FM4_RESOLVE_RESIDENT_COVER:-1}" \
  FM4_TEXTURE_GPU_UNTILE="${FM4_TEXTURE_GPU_UNTILE:-0}" FM4_VERTEX_CACHE_MB="${FM4_VERTEX_CACHE_MB:-256}" FM4_SUBMIT_IF_IDLE="${FM4_SUBMIT_IF_IDLE:-8}" \
  FM4_BULK_REGISTERS="${FM4_BULK_REGISTERS:-1}" FM4_ASYNC_RETIRE="${FM4_ASYNC_RETIRE:-1}"
# FM4_NO_DRAIN (2026-11-25, ShiftGlue's command-processor model): fences and interrupts reach the game when
# the command processor reaches them, not after the GPU drains; only a resolve that is not a steady capture is waited for. +14 % (occlusion
# off) to +24 % (on) on the parked QHD yardstick; menus, race load, driving and photo mode rendered the same. FM4_NO_DRAIN=0 restores the drain.
export FM4_NO_DRAIN="${FM4_NO_DRAIN:-1}"
# Keyboard prompts (runtime/game/prompt_text.cpp): after a key press the game's button prompts name the bound keys ("Press SPACE to
# start"), after a pad input the pad's buttons again. FM4_KEYBOARD_PROMPTS=0 keeps the game's own words.
export FM4_KEYBOARD_PROMPTS="${FM4_KEYBOARD_PROMPTS:-1}"
# Keycap icons (runtime/video/keycap.h): with the keyboard prompts, the pad's button icons become keycaps of the bound keys.
# FM4_KEYCAP_ICONS=0 keeps the pad's icons.
export FM4_KEYCAP_ICONS="${FM4_KEYCAP_ICONS:-1}"
# The four speed settings of the internal resolutions (validated at 720p, Full HD, QHD and 4K, the held
# main menu byte-identical with and without them): the scaled pitch and the fused resolves act only above 720p. 0 turns one off.
export FM4_INTERNAL_SCALE_PITCH="${FM4_INTERNAL_SCALE_PITCH:-1}" FM4_RESOLVE_FUSED="${FM4_RESOLVE_FUSED:-1}" FM4_PRESENT_LAZY="${FM4_PRESENT_LAZY:-1}" \
  FM4_EDRAM_RELEASE_REPLACED="${FM4_EDRAM_RELEASE_REPLACED:-1}"
if [ "$FM4_VERTEX_CACHE_MB" = 0 ]; then unset FM4_VERTEX_CACHE_MB; fi  # 0: no vertex cache (the game refuses a size outside 64-1024)
# Rendering resolution (the Settings panel, Command-comma in the game's menus (--controls-panel below), writes $FM4_RECOMP_HOME/graphics.conf: internal_resolution =
# 720p | fhd | qhd | 4k): 720p is the game's own 1280x720; fhd renders and shows 1920x1080 and is the default, qhd 2560x1440, 4k
# 3840x2160. A value already
# in the environment wins. No file is fhd; so is a file with any other value (the game would refuse to start on one), named here.
RES=fhd
if [ -n "${FM4_INTERNAL_RES:-}" ]; then
  RES="$FM4_INTERNAL_RES"
elif [ -f "$ROOT/graphics.conf" ]; then
  value="$(sed -n 's/^[[:space:]]*internal_resolution[[:space:]]*=[[:space:]]*\([^[:space:]]*\)[[:space:]]*$/\1/p' "$ROOT/graphics.conf" | tail -1)"
  case "$value" in
    720p|fhd|qhd|4k) RES="$value" ;;
    "") ;;  # the file names no resolution
    *) echo "play: graphics.conf: unknown resolution '$value': using fhd" >&2 ;;
  esac
fi
export FM4_INTERNAL_RES="$RES"
# Extra edge smoothing (the same panel and file: anti_aliasing = off | soft | on | strong, on being the Standard strength;
# runtime/video/post_aa.h, a post-process pass after the game's own anti-aliasing): on (Standard) unless the file says otherwise.
# A value already in the environment wins; any other value in the file is on, named here (the game would refuse to start on one).
AA=on
if [ -n "${FM4_POST_AA:-}" ]; then
  AA="$FM4_POST_AA"
elif [ -f "$ROOT/graphics.conf" ]; then
  value="$(sed -n 's/^[[:space:]]*anti_aliasing[[:space:]]*=[[:space:]]*\([^[:space:]]*\)[[:space:]]*$/\1/p' "$ROOT/graphics.conf" | tail -1)"
  case "$value" in
    on|off|soft|strong) AA="$value" ;;
    "") ;;  # the file does not name it
    *) echo "play: graphics.conf: unknown anti_aliasing '$value': using on" >&2 ;;
  esac
fi
export FM4_POST_AA="$AA"
# HDR output (the same panel and file: hdr = auto | off; runtime/video/hdr_output.h): off unless the file says auto. A value already
# in the environment wins; any other value in the file is off, named here. Auto is HDR only where the display and the system can
# show it: otherwise the game says why and presents in standard range. The GRAPHICS menu and the Settings panel switch it while
# the game runs (FM4_GRAPHICS_LIVE).
HDR=off
if [ -n "${FM4_HDR:-}" ]; then
  HDR="$FM4_HDR"
elif [ -f "$ROOT/graphics.conf" ]; then
  value="$(sed -n 's/^[[:space:]]*hdr[[:space:]]*=[[:space:]]*\([^[:space:]]*\)[[:space:]]*$/\1/p' "$ROOT/graphics.conf" | tail -1)"
  case "$value" in
    auto|off) HDR="$value" ;;
    "") ;;  # the file does not name it
    *) echo "play: graphics.conf: unknown hdr '$value': using off" >&2 ;;
  esac
fi
export FM4_HDR="$HDR"
# The sun's flare follows the sun's visibility (real occlusion queries, runtime/video/zpd_report.h): on (validated at 720p, Full HD, QHD and 4K with no measurable cost, research/m9/glare/RESULTS.md). 0: the fixed count as before.
export FM4_OCCLUSION_QUERIES="${FM4_OCCLUSION_QUERIES:-1}"
# A view setting that is not offered in this release (runtime/kernel/extended_view.h): off, its file line not read and its title
# hooks not armed unless FM4_EXTENDED_VIEW_OFFERED=1 (then: extended_view = on | off in the file, a value already in the
# environment wins, any other value in the file is off and named here).
export FM4_EXTENDED_VIEW_OFFERED="${FM4_EXTENDED_VIEW_OFFERED:-0}"
EXTENDED=off
if [ "$FM4_EXTENDED_VIEW_OFFERED" != 1 ]; then
  :
elif [ -n "${FM4_EXTENDED_VIEW:-}" ]; then
  EXTENDED="$FM4_EXTENDED_VIEW"
elif [ -f "$ROOT/graphics.conf" ]; then
  value="$(sed -n 's/^[[:space:]]*extended_view[[:space:]]*=[[:space:]]*\([^[:space:]]*\)[[:space:]]*$/\1/p' "$ROOT/graphics.conf" | tail -1)"
  case "$value" in
    on|off) EXTENDED="$value" ;;
    "") ;;  # the file does not name it
    *) echo "play: graphics.conf: unknown extended_view '$value': using off" >&2 ;;
  esac
fi
export FM4_EXTENDED_VIEW="$EXTENDED"
if [ "$FM4_EXTENDED_VIEW_OFFERED" = 1 ]; then export FM4_EXTENDED_VIEW_HOOK="${FM4_EXTENDED_VIEW_HOOK:-1}"; else export FM4_EXTENDED_VIEW_HOOK=0; fi
# The game's own GRAPHICS menu and the Settings panel apply the resolution, edge smoothing and HDR while the game runs
# (runtime/video/renderer.cpp); 0 turns that off (a choice then applies at the next start).
export FM4_GRAPHICS_LIVE="${FM4_GRAPHICS_LIVE:-1}"
# FM4 Recomp (render scenario settings): the GRAPHICS menu's SCENERY SHADOWS ... LEVEL OF DETAIL and DEPTH OF FIELD rows and their title
# hooks (config/title_hooks.toml; runtime/game/render_scenario.cpp). The game reads their values from graphics.conf itself (each line
# only when not the game's own), so nothing is exported for them here. On since a bounded run with every one at its maximum (drive
# route, 2026-11-26: applied, no stop, no failed allocation); 0 hides the rows and leaves the game's values.
export FM4_RENDER_SCENARIO="${FM4_RENDER_SCENARIO:-1}" FM4_DEPTH_OF_FIELD_HOOK="${FM4_DEPTH_OF_FIELD_HOOK:-1}"
# MOTION BLUR and CAR DIRT (runtime/video/shader_overrides.h): pixel-shader constants of FM4's road detail and car shaders, set by the
# renderer when a row is OFF (graphics.conf motion_blur / car_dirt). 0 hides the rows.
export FM4_SHADER_OVERRIDES="${FM4_SHADER_OVERRIDES:-1}"
# Persistent shader and pipeline caches (FM4_SHADER_CACHE: compiled SPIR-V and the Vulkan pipeline cache, ~35 MB after a race) in the
# data directory, so a shader's first-use compile stall happens once, not every session. FM4_SHADER_CACHE=0 turns them off; another
# path moves them.
FM4_SHADER_CACHE="${FM4_SHADER_CACHE:-$DATA/shader_cache}"
if [ "$FM4_SHADER_CACHE" = 0 ]; then unset FM4_SHADER_CACHE; else export FM4_SHADER_CACHE; fi
export FM4_SHADER_DUMP="$LOGS/shaders"  # a refused shader's microcode (<ps|vs>_<hash>.bin) for the developers' offline replay (alpha)
# FM4_PLAYTEST=1 (acceptance playthroughs): stall stamps (FM4_DIAG_SLOW_SUBMIT: long swaps, long fence waits, slow submits, with
# wall-clock times), a kept capture every FM4_CAPTURE_KEEP_EVERY swaps (default 3000; captures/), the macOS thermal and performance
# warning levels every 30 s (thermal.txt), and after the session coreaudiod and RunningBoard events (syslog_playtest.txt).
PLAYTEST=0
if [ "${FM4_PLAYTEST:-0}" = 1 ]; then
  PLAYTEST=1
  export FM4_DIAG_SLOW_SUBMIT=1 FM4_CAPTURE_KEEP_EVERY="${FM4_CAPTURE_KEEP_EVERY:-3000}"
fi
# FM4_RECOMP_AUDIO=mute: no audio device is opened (test sessions on a shared Mac); otherwise the default output device.
AUDIO=audible
[ "${FM4_RECOMP_AUDIO:-}" = mute ] && AUDIO=mute
# The game's window, its Disc 2 panel and Settings panel, and the live pad; the warm-up: offscreen, muted, the route's scripted pad.
GAME_MODE=(--renderer window --disc-setup "$HERE/bin/fm4recomp-setup" --controls-panel "$HERE/bin/fm4recomp-setup")
if [ "$WARMUP" = 1 ]; then
  AUDIO=mute
  unset FM4_INPUT_LIVE
  export FM4_INPUT_SCRIPT="$ROUTE"
  GAME_MODE=(--renderer present)
fi
USER_HOME="$HOME"
export HOME="$DATA/home"
echo "play: starting (session limit $MINUTES min; rendering resolution $RES, edge smoothing $AA; logs in $LOGS)"
since=$(date +%s); touch "$LOGS/start"
THERMAL_PID=""
if [ "$PLAYTEST" = 1 ]; then
  ( while :; do printf '%s ' "$(date '+%H:%M:%S')"; /usr/bin/pmset -g therm 2>&1 | tr '\n' ' '; echo; sleep 30; done ) > "$LOGS/thermal.txt" 2>&1 &
  THERMAL_PID=$!
fi
status=0
WATCH_PID=""
if [ "$WARMUP" = 1 ]; then
  # Progress from the route's sections as the game logs them; at the route's last event (its last @scene list's last line, a release
  # some 20 s of menu after the last race, past a pipeline-cache save) the game is stopped and the warm-up is complete.
  races="$(grep -c '^@scene' "$ROUTE")"
  last="$(grep -Ev '^[[:space:]]*(#|$)' "$ROUTE" | tail -1 | awk '{print $1}')"
  echo "progress 0 Starting the game"
  # No sleep while it runs: muted, nothing else holds the Mac awake (a played game's audio does), and an idle sleep mid-route stopped
  # a test warm-up with an in-process GPU_STOP (a fence wait expired across the sleep, 2026-11-27).
  /usr/bin/caffeinate -i -w $$ &
  ( seen=0
    while sleep 2; do
      n="$(grep -c '^\[input\] @scene' "$LOGS/game.log" 2>/dev/null || true)"
      if [ "${n:-0}" -gt "$seen" ]; then
        seen=$n; echo "progress $((100 * (n - 1) / races + 2)) Warming up shaders: race $n of $races"
      fi
      if [ "$seen" = "$races" ] && grep -q "at scene vblank $last: released" "$LOGS/game.log"; then
        touch "$LOGS/warm-up-complete"; pkill -TERM -x fm4recomp || true; break
      fi
    done ) &
  WATCH_PID=$!
fi
run_game() {
perl -e '
  my ($deadline, @cmd) = @ARGV;
  my $pid = fork; die "fork: $!" unless defined $pid;
  if (!$pid) { setpgrp(0, 0); exec { $cmd[0] } @cmd; die "exec: $!\n"; }
  $SIG{ALRM} = sub { kill "TERM", -$pid; sleep 2; kill "KILL", -$pid; waitpid $pid, 0; print STDERR "[play] session limit: stopped\n"; exit 124 };
  alarm $deadline + 30; waitpid $pid, 0; alarm 0;
  my $s = $?; exit($s & 127 ? 128 + ($s & 127) : $s >> 8);' \
  "$DEADLINE" "$HERE/bin/fm4recomp" "${LOC_GAME_ARGS[@]}" --gpu-stop "$STOP_FLAG" --audio "$AUDIO" \
  "${GAME_MODE[@]}" --gpu-capture "$LOGS/captures" --vk-validation "$VALIDATION" --shader-classes branch,predicate,loop,dynloop \
  --stop-at-swap 100000000 --deadline "$DEADLINE" > "$LOGS/game.out" 2> "$LOGS/game.log"
}
if [ "$WARMUP" = 1 ]; then
  # A stop of the warm-up (the setup app's Skip: TERM to this script) reaches the game; bash runs the trap during wait, not while a
  # foreground command runs. The shaders compiled so far stay in the cache.
  run_game &
  LAUNCH=$!
  trap 'pkill -TERM -x fm4recomp 2>/dev/null || true' TERM INT
  while :; do
    wait "$LAUNCH" && { status=0; break; } || status=$?
    kill -0 "$LAUNCH" 2>/dev/null || break
  done
  trap - TERM INT
else
  run_game || status=$?
fi
echo "play: the game ended (exit $status): $(grep -m1 '^\[stop ' "$LOGS/game.log" || echo 'no stop line')"
if [ -n "$WATCH_PID" ]; then kill "$WATCH_PID" 2>/dev/null || true; wait "$WATCH_PID" 2>/dev/null || true; fi
if [ -n "$THERMAL_PID" ]; then kill "$THERMAL_PID" 2>/dev/null || true; fi
if [ "$PLAYTEST" = 1 ]; then
  /usr/bin/log show --style compact --start "$(date -r "$since" '+%Y-%m-%d %H:%M:%S')" \
    --predicate 'process == "coreaudiod" OR (process == "runningboardd" AND eventMessage CONTAINS "fm4recomp")' > "$LOGS/syslog_playtest.txt" 2>/dev/null || true
fi
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
if [ "$VALIDATION" = on ]; then
  grep -q 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$LOGS/game.log" ||
    echo "play: WARNING: debug mode, but the game did not load the validation layer; please send $LOGS/game.log to the developers" >&2
elif grep -q 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$LOGS/game.log"; then
  echo "play: WARNING: a validation layer loaded in normal mode (something outside FM4 Recomp inserted it)" >&2
fi
if grep -E 'Validation (Error|Warning)|\[vulkan validation\]' "$LOGS/game.log" | head -5; then
  echo "play: the validation layer reported problems (above); please send $LOGS/game.log to the developers" >&2
fi
if [ "$WARMUP" = 1 ]; then
  if [ -e "$LOGS/warm-up-complete" ]; then
    echo "progress 100 Shaders warmed up"
    echo "play: warm-up complete, no GPU hang evidence. Log: $LOGS/game.log"; exit 0
  fi
  echo "play: the warm-up stopped before the route's end (exit $status); the shaders compiled so far are kept. Log: $LOGS/game.log" >&2
  exit 1
fi
echo "play: session over, no GPU hang evidence. Log: $LOGS/game.log"
