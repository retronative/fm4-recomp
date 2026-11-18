// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: the window build's Keyboard Controls screen (kernel/controls_screen.h): the launcher's Keyboard Controls
// (`<panel> --controls-panel --frame <x,y,w,h>`, app/setup/ControlsPanel.swift) as a child process in its own process group,
// opened centred over the game window; its output goes to the game's log. As disc_setup_sdl.cpp, nothing here waits: the
// window loop polls the child's end. Not built into the CPU tests (they fake the hooks).
#pragma once
#include "kernel/controls_screen.h"
#include <string>

namespace runtime::controls {

// The hooks for `panel` (fm4recomp --controls-panel; play.sh passes the kit's bin/fm4recomp-setup), the bindings file in
// $FM4_RECOMP_HOME; `systemUi` and `busy` are main.cpp's (they need the guest base and the other screens). Stops (9) when `panel`
// is not an executable or FM4_RECOMP_HOME is not set.
Hooks MakeSdlHooks(const std::string& panel, std::function<void(bool)> systemUi, std::function<const char*()> busy);

}  // namespace runtime::controls
