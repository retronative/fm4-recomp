# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Storage locations of the FM4 Recomp alpha, shared by setup.sh and play.sh (sourced; bash 3.2, macOS's /bin/bash). The player
# may keep the game data and the installed Disc 2 content anywhere, an external SSD included; the internal location stays the
# default. The choice lives in one file, $FM4_RECOMP_HOME/locations.conf, which setup.sh writes and play.sh and fm4recomp read:
#
#   # FM4 Recomp storage locations (written by setup.sh)
#   version=1
#   data_root=/Volumes/Extreme SSD/FM4 Recomp/data        Disc 1's files/, survey/ (also disc2_gdfx.tsv), modules/, home/
#   user_data_root=/Users/me/Library/Application Support/FM4 Recomp/data/user-data   --user-data: saves and installed content
#   disc2_root=/Volumes/Extreme SSD/FM4 Recomp/disc2      --disc2 (only once setup.sh --disc2 has finished)
#   disc2_map=/Volumes/Extreme SSD/FM4 Recomp/data/survey/disc2_gdfx.tsv   --disc2-map (with disc2_root)
#
# Format: UTF-8 text, one key=value per line; the value is the rest of the line after the first '=', taken verbatim (an absolute
# path: no quoting, may hold spaces and '=', never a newline, CR or NUL); '#' lines and blank lines are comments; a key appears at
# most once (readers refuse a duplicate); readers ignore unknown keys; a missing key or file means the default:
# data_root = $FM4_RECOMP_HOME/data, user_data_root = <data_root default>/user-data, no Disc 2. Written atomically (temporary
# file, then rename) and only after the step it records has passed its checks.

LOC_FILE_NAME=locations.conf
LOC_MARGIN_BYTES=1073741824   # kept free on every destination volume beyond what a step writes

loc_file() { printf '%s/%s\n' "$1" "$LOC_FILE_NAME"; }

# loc_get <home> <key>: the recorded value, or nothing; exit 1 when the key appears twice.
loc_get() {
  local file; file="$(loc_file "$1")"
  [ -f "$file" ] || return 0
  awk -v key="$2" 'index($0, key "=") == 1 { n++; value = substr($0, length(key) + 2) }
    END { if (n > 1) exit 1; if (n == 1) print value }' "$file" ||
    { echo "locations: $file names $2 more than once" >&2; return 1; }
}

# loc_resolve <home>: sets LOC_DATA_ROOT, LOC_USER_DATA_ROOT, LOC_DISC2_ROOT, LOC_DISC2_MAP (the last two empty without Disc 2).
loc_resolve() {
  LOC_DATA_ROOT="$(loc_get "$1" data_root)" || return 1
  LOC_USER_DATA_ROOT="$(loc_get "$1" user_data_root)" || return 1
  LOC_DISC2_ROOT="$(loc_get "$1" disc2_root)" || return 1
  LOC_DISC2_MAP="$(loc_get "$1" disc2_map)" || return 1
  [ -n "$LOC_DATA_ROOT" ] || LOC_DATA_ROOT="$1/data"
  [ -n "$LOC_USER_DATA_ROOT" ] || LOC_USER_DATA_ROOT="$1/data/user-data"
  if { [ -n "$LOC_DISC2_ROOT" ] && [ -z "$LOC_DISC2_MAP" ]; } || { [ -z "$LOC_DISC2_ROOT" ] && [ -n "$LOC_DISC2_MAP" ]; }; then
    echo "locations: $(loc_file "$1") names only one of disc2_root and disc2_map" >&2; return 1
  fi
}

# loc_valid <path>: an absolute path without newline or CR (NUL cannot occur in a bash string).
loc_valid() {
  case "$1" in
    /*) ;;
    *) echo "locations: $1 is not an absolute path" >&2; return 1 ;;
  esac
  case "$1" in
    *$'\n'* | *$'\r'*) echo "locations: a path with a line break is refused" >&2; return 1 ;;
  esac
}

# loc_abs <path>: absolute (relative to the current directory), without a trailing slash; the path need not exist.
loc_abs() {
  local p="$1"
  [ -n "$p" ] || { echo "locations: an empty path" >&2; return 1; }
  case "$p" in /*) ;; *) p="$PWD/$p" ;; esac
  while [ "$p" != / ] && [ "${p%/}" != "$p" ]; do p="${p%/}"; done
  loc_valid "$p" && printf '%s\n' "$p"
}

# loc_set <home> <key> <value> [<key> <value> ...]: records the values, keeping every other line, atomically.
loc_set() {
  local home="$1" file tmp args=() k
  shift
  file="$(loc_file "$home")"
  while [ $# -ge 2 ]; do
    case "$1" in *[!a-z0-9_]* | '') echo "locations: bad key $1" >&2; return 1 ;; esac
    loc_valid "$2" || return 1
    args+=("$1" "$2")
    shift 2
  done
  [ $# = 0 ] || { echo "locations: loc_set needs key value pairs" >&2; return 1; }
  mkdir -p "$home"
  tmp="$file.tmp.$$"
  {
    if [ -f "$file" ]; then
      awk -v keys="$(for ((k = 0; k < ${#args[@]}; k += 2)); do printf '%s,' "${args[$k]}"; done)" '
        BEGIN { n = split(keys, list, ","); for (i = 1; i <= n; i++) if (list[i] != "") drop[list[i] "="] = 1 }
        { for (k in drop) if (index($0, k) == 1) next; print }' "$file"
    else
      echo "# FM4 Recomp storage locations (written by setup.sh; read by play.sh and fm4recomp). Format: locations.sh."
      echo "version=1"
    fi
    for ((k = 0; k < ${#args[@]}; k += 2)); do printf '%s=%s\n' "${args[$k]}" "${args[$((k + 1))]}"; done
  } > "$tmp"
  mv "$tmp" "$file"
}

# loc_existing <path>: the nearest existing ancestor (the volume a not-yet-created folder will be on).
loc_existing() {
  local p="$1"
  while [ ! -e "$p" ] && [ "$p" != / ]; do p="$(dirname "$p")"; done
  printf '%s\n' "$p"
}

# loc_df <path>: "<free KiB> <mount point>" of the volume holding <path> (df -Pk; the mount point may contain spaces).
loc_df() {
  df -Pk "$(loc_existing "$1")" | awk 'NR == 2 { m = $6; for (i = 7; i <= NF; i++) m = m " " $i; print $4 " " m }'
}

# loc_need <path> <bytes> [<path> <bytes> ...]: each volume must have the bytes of every destination on it plus
# LOC_MARGIN_BYTES free. Prints "locations: ..." naming the short volume and returns 1 otherwise.
loc_need() {
  local mounts=() needs=() frees=() paths=() i found line free mount
  while [ $# -ge 2 ]; do
    line="$(loc_df "$1")" || return 1
    free="${line%% *}"; mount="${line#* }"
    found=-1
    for ((i = 0; i < ${#mounts[@]}; i++)); do [ "${mounts[$i]}" = "$mount" ] && found=$i; done
    if [ "$found" = -1 ]; then
      mounts+=("$mount"); needs+=("$2"); frees+=("$free"); paths+=("$1")
    else
      needs[$found]=$((needs[found] + $2)); paths[$found]="${paths[$found]}, $1"
    fi
    shift 2
  done
  for ((i = 0; i < ${#mounts[@]}; i++)); do
    if [ $((frees[i] * 1024)) -lt $((needs[i] + LOC_MARGIN_BYTES)) ]; then
      echo "locations: not enough free space on ${mounts[$i]} (for ${paths[$i]}): $((frees[i] / 1024 / 1024)) GB free, $(((needs[i] + LOC_MARGIN_BYTES + 1073741823) / 1073741824)) GB needed" >&2
      return 1
    fi
  done
}

# loc_game_args <home>: sets LOC_GAME_ARGS, fm4recomp's data arguments from the recorded locations (play.sh), after checking that
# each is there: a missing folder on a chosen drive is refused (never recreated empty, which would hide the player's saves); the
# internal default user-data folder is created. Disc 2's two arguments only once setup.sh --disc2 has recorded them;
# --keybindings only when <home>/keybindings.conf exists (the game checks its contents).
loc_game_args() {
  local f
  loc_resolve "$1" || return 1
  for f in "$LOC_DATA_ROOT/survey/image.bin" "$LOC_DATA_ROOT/files/default.xex" "$LOC_DATA_ROOT/survey/disc1_gdfx.tsv"; do
    [ -e "$f" ] || { echo "locations: $f is missing: run ./setup.sh with your own Disc 1 image first (or connect its drive)" >&2; return 1; }
  done
  if [ ! -d "$LOC_USER_DATA_ROOT" ]; then
    [ "$LOC_USER_DATA_ROOT" = "$1/data/user-data" ] ||
      { echo "locations: the user-data folder $LOC_USER_DATA_ROOT is missing: is its drive connected?" >&2; return 1; }
    mkdir -p "$LOC_USER_DATA_ROOT"
  fi
  LOC_GAME_ARGS=(--image "$LOC_DATA_ROOT/survey/image.bin" --xex "$LOC_DATA_ROOT/files/default.xex" --disc "$LOC_DATA_ROOT/files"
                 --disc-map "$LOC_DATA_ROOT/survey/disc1_gdfx.tsv")
  if [ -n "$LOC_DISC2_ROOT" ]; then
    for f in "$LOC_DISC2_ROOT/default.xex" "$LOC_DISC2_MAP"; do
      [ -e "$f" ] || { echo "locations: $f is missing although Disc 2 was set up: connect its drive, or run ./setup.sh --disc2 again" >&2; return 1; }
    done
    LOC_GAME_ARGS+=(--disc2 "$LOC_DISC2_ROOT" --disc2-map "$LOC_DISC2_MAP")
  fi
  LOC_GAME_ARGS+=(--modules "$LOC_DATA_ROOT/modules" --user-data "$LOC_USER_DATA_ROOT")
  # The keyboard bindings the in-game Keyboard Controls saved; none: the game's defaults (fm4recomp re-reads this path when it closes).
  if [ -e "$1/keybindings.conf" ]; then
    LOC_GAME_ARGS+=(--keybindings "$1/keybindings.conf")
  fi
}
