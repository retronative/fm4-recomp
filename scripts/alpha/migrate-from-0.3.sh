#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# Brings the saved games kept by FM4 Recomp 0.3.x back into this engine's layout (migrate-from-0.3.swift, bin/fm4_migrate_from_03).
# play.sh runs it under its per-profile lock, before the game mounts the user-data folder:
#   migrate-from-0.3.sh <user-data folder>
set -euo pipefail
kit="$(cd "$(dirname "$0")" && pwd)"
exec "$kit/bin/fm4_migrate_from_03" "$@"
