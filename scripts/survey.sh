#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Survey and recompile one of FM4's modules: the M1 pipeline (survey/survey.py does the work).
#
#   scripts/survey.sh [--module default|xmediafacade|speechfacade] [--diagnostic] [--compare-with <run1>.json]
#
# Without --module: the executable (config/fm4.toml, game/files/default.xex); xmediafacade:
# config/xmediafacade.toml, game/files/XMediaFacade_default.xex, outputs under game/modules/xmediafacade/.
#
# Refuses to start unless scripts/preflight.sh says the machine is free; prints the SHA-256 of
# every tool and input before the run (a run once used a stale binary). Bounded: each step of
# survey/survey.py has a deadline. Fail-closed: exit 1 on any recompiler problem, error marker,
# inconsistent input or (with --compare-with) any byte difference from the earlier run.
# Two-run reproducibility check (M1 acceptance):
#   scripts/survey.sh && cp game/survey/reproducibility.json research/m1/repro_run1.json
#   scripts/survey.sh --compare-with research/m1/repro_run1.json
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY=/opt/homebrew/bin/python3
cd "$ROOT"

"$ROOT/scripts/preflight.sh"
[ -x "$PY" ] || { echo "survey: $PY is missing (Python 3.11+ with tomllib)" >&2; exit 1; }
CONFIG=config/fm4.toml XEX=game/files/default.xex
for ((i = 1; i <= $#; i++)); do
  if [ "${!i}" = --module ]; then
    j=$((i + 1))
    case "${!j:-}" in
      default) ;;
      xmediafacade) CONFIG=config/xmediafacade.toml XEX=game/files/XMediaFacade_default.xex ;;
      speechfacade) CONFIG=config/speechfacade.toml XEX=game/files/SpeechFacade_default.xex ;;
      *) echo "survey: --module needs default, xmediafacade or speechfacade" >&2; exit 1 ;;
    esac
  fi
done
for f in tools/survey/xexsurvey tools/XenonRecomp/build/XenonRecomp/XenonRecomp \
         tools/XenonRecomp/XenonUtils/ppc_context.h "$CONFIG" "$XEX"; do
  [ -e "$f" ] || { echo "survey: $f is missing (scripts/setup_tools.sh, scripts/extract_game.sh)" >&2; exit 1; }
  shasum -a 256 "$f"
done
echo "XenonRecomp patch series: $(git -C tools/XenonRecomp log --oneline -1)"
exec "$PY" -B -I survey/survey.py "$@"
