#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
project=$(cd "$(dirname "$0")/.." && pwd)
kit=$(mktemp -d)
trap 'rm -rf "$kit"' EXIT
ROOT="$kit/Custom Home"
LOC_DATA_ROOT="$kit/disc data"
LOC_USER_DATA_ROOT="$kit/separate saves"
HERE="$kit"
LOGS="$ROOT/logs"
AUDIO=mute VALIDATION=off
mkdir -p "$ROOT" "$LOC_USER_DATA_ROOT" "$kit/engines/shiftglue" "$kit/bin"
touch "$kit/engines/shiftglue/fm4_static_recomp" "$kit/engines/shiftglue/librexgpu-fh1.dylib"
chmod +x "$kit/engines/shiftglue/fm4_static_recomp"
for helper in migrate-career.sh migrate-playtest-profile.sh; do
  printf '#!/bin/bash\nexit 0\n' > "$kit/$helper"
  chmod +x "$kit/$helper"
done
source "$project/scripts/alpha/shiftglue-playtest.sh"
configure_shiftglue
for arg in "--user_data_root=$LOC_USER_DATA_ROOT" "--cache_root=$ROOT/cache" "--fm4_config_root=$ROOT/config" "--update_data_root=$ROOT/update"; do
 printf '%s\n' "${GAME_CMD[@]}" | grep -Fxq -- "$arg" || { echo "FAIL missing $arg"; exit 1; }
done
[ "$GAME_HOME" = "$ROOT/home" ] && [ ! -e "$ROOT/shiftglue" ]
echo 'PASS: configured save location and production profile paths'
