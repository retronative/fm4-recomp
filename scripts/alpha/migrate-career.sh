#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run under the launcher's per-profile lock before the game mounts this user root.
set -eu
kit="$(cd "$(dirname "$0")" && pwd)"
exec "$kit/bin/fm4_migrate_career" "$@"
