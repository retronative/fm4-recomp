#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by play.sh after its install, device, session and graphics checks.
configure_shiftglue() {
  local engine="$HERE/engines/shiftglue" profile="$ROOT" muted=false
  local headless=false validation=false
  if [ ! -x "$engine/fm4_static_recomp" ] || [ ! -f "$engine/librexgpu-fh1.dylib" ]; then
    echo "play: this kit does not include the game engine" >&2
    return 1
  fi
  if [ "${FM4_COMPARE_OFFSCREEN:-0}" = 1 ]; then
    [ "$AUDIO" = mute ] || { echo "play: offscreen trials require muted audio" >&2; return 2; }
    headless=true
    unset FM4_REFERENCE_WINDOW_OUTPUT
    export SDL_MAC_BACKGROUND_APP=1
  else
    export FM4_REFERENCE_WINDOW_OUTPUT=1
    unset FM4_BENCH_MATCH_OUTPUT FM4_STATIC_RECOMP_INPUT_SCRIPT FM4_COMPARE_CAPTURE_DIR
  fi
  [ "$AUDIO" != mute ] || muted=true
  if [ "$VALIDATION" = on ]; then
    validation=true
    export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
  fi
  GAME_HOME="$profile/home"
  mkdir -p "$GAME_HOME" "$LOC_USER_DATA_ROOT" "$profile/cache" "$profile/update" "$profile/config" || return 1
  /bin/bash "$HERE/migrate-career.sh" "$LOC_USER_DATA_ROOT" || return 1
  if [ "$ROOT" = "$HOME/Library/Application Support/FM4 Recomp" ]; then
    /bin/bash "$HERE/migrate-playtest-profile.sh" \
      "$HOME/Library/Application Support/FM4 Recomp ShiftGlue Playtest" "$ROOT" "$LOC_USER_DATA_ROOT" || return 1
  fi
  # ApplySavedGraphicsSettings can replace SDK environment defaults with the player's saved values.
  export REX_FM4_FRAME_RATE="${REX_FM4_FRAME_RATE:-60}"
  export REX_FM4_LEVEL_OF_DETAIL="${REX_FM4_LEVEL_OF_DETAIL:-console}"
  export REX_FM4_SHOWROOM_MSAA="${REX_FM4_SHOWROOM_MSAA:-4x}"
  export REX_FM4_RACE_MSAA="${REX_FM4_RACE_MSAA:-2x}"
  # Native Full HD uses the 2x backing; the host restores saved selections.
  export REX_RESOLUTION_SCALE="${REX_RESOLUTION_SCALE:-2}"
  export REX_FM4_NATIVE_FHD="${REX_FM4_NATIVE_FHD:-true}"
  export REX_FM4_AUTO_FSR=true
  export REX_SWAP_POST_EFFECT="${REX_SWAP_POST_EFFECT:-fxaa}"
  GAME_CMD=("$engine/fm4_static_recomp"
    "--game_data_root=$LOC_DATA_ROOT/files" "--user_data_root=$LOC_USER_DATA_ROOT"
    "--cache_root=$profile/cache" "--update_data_root=$profile/update" "--fm4_config_root=$profile/config"
    "--log_file=$LOGS/shiftglue.log" --gpu_backend=vulkan --video_driver=cocoa
    "--headless=$headless" "--audio_mute=$muted" "--vulkan_validation_enabled=$validation"
    --unprotected_page_check_interval=1 --present_effect=fsr --swap_post_effect_scene_only=false)
  if [ "${FM4_SHIFTGLUE_OPTIMIZE:-0}" = 1 ]; then
    export FM4_REFERENCE_STANDARD_TILE=1
    GAME_CMD+=(--fh1_specialize_edram_passes=true --fh1_async_specialized_passes=true --fh1_native_stencil_export=true)
    echo "play: tiled Standard AA, specialized EDRAM passes and native stencil export requested"
  fi
  if [ "$headless" = true ]; then
    GAME_CMD+=(--fm4_skip_intro=true --fh1_native_gpu_profile=false --perf_critical_path_trace=true)
  else
    export REX_PRESENT_HDR="${REX_PRESENT_HDR:-true}"
    GAME_CMD+=(--present_hdr_paper_white_nits=80 --present_hdr_peak_nits=160
      --present_hdr_highlights=1 --present_hdr_gamma=0)
  fi
  echo "play: ShiftGlue engine, graphics profile in $profile/config, HDR Auto, saves in $LOC_USER_DATA_ROOT"
}
