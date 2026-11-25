#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 FM4 Recomp contributors
#
# FM4 Recomp.app's pure logic, headless: the keyboard bindings' key table, file, parse rules, conflicts and save
# (tests/app/test_keybindings.swift over app/setup/KeyBindings.swift), the graphics settings' file
# (tests/app/test_graphics_settings.swift over app/setup/GraphicsSettings.swift), and what opening the app does: setup, repair, blocked or play
# (tests/app/test_launch_decision.swift over app/setup/Core.swift, KeyBindings.swift and LaunchDecision.swift). No window, no SwiftUI; the compiler
# flags match the launcher. The profile checks use temporary homes, save packages and disc markers.
#
#   tests/run_app_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/preflight.sh" >/dev/null
OUT="$ROOT/build/tests/app"
mkdir -p "$OUT"
swiftc=(/usr/bin/xcrun swiftc -swift-version 5 -target arm64-apple-macos26.0 -parse-as-library -module-cache-path "$OUT/modules")
perl -e 'alarm 300; exec @ARGV' "${swiftc[@]}" -module-name KeyBindingsTest \
  "$ROOT/app/setup/KeyBindings.swift" "$ROOT/tests/app/test_keybindings.swift" -o "$OUT/test_keybindings"
perl -e 'alarm 300; exec @ARGV' "${swiftc[@]}" -module-name LaunchDecisionTest \
  "$ROOT/app/setup/Core.swift" "$ROOT/app/setup/KeyBindings.swift" "$ROOT/app/setup/LaunchDecision.swift" "$ROOT/tests/app/test_launch_decision.swift" -o "$OUT/test_launch_decision"
perl -e 'alarm 300; exec @ARGV' "${swiftc[@]}" -module-name GraphicsSettingsTest \
  "$ROOT/app/setup/GraphicsSettings.swift" "$ROOT/tests/app/test_graphics_settings.swift" -o "$OUT/test_graphics_settings"
perl -e 'alarm 300; exec @ARGV' "${swiftc[@]}" -module-name ProfileSetupTest \
  "$ROOT/app/setup/Core.swift" "$ROOT/app/setup/ProfileSetup.swift" "$ROOT/tests/app/test_profile_setup.swift" -o "$OUT/test_profile_setup"
status=0
perl -e 'alarm 60; exec @ARGV' "$OUT/test_graphics_settings" || status=$?
perl -e 'alarm 60; exec @ARGV' "$OUT/test_keybindings" || status=$?
perl -e 'alarm 60; exec @ARGV' "$OUT/test_launch_decision" || status=$?
perl -e 'alarm 60; exec @ARGV' "$OUT/test_profile_setup" || status=$?
/bin/bash "$ROOT/tests/test_production_profile.sh" || status=$?
python3 "$ROOT/tests/test_migrate_career.py" || status=$?
python3 "$ROOT/tests/test_migrate_playtest_profile.py" || status=$?
if [ "$status" = 0 ] && [ -z "${KEEP_WORK:-}" ]; then rm -r "$OUT"; fi
exit $status
