// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (Disc 2): the player's side of a pending disc swap (kernel/disc_swap.h Host) in
// the window build: the launcher's Disc setup panel (FM4 Recomp's setup app in --disc-panel mode, app/setup/Panel.swift)
// as a child process in its own process group, opened centred over the game window; it runs the kit's setup tool itself
// (`setup.sh --disc2 [--dest <dir>] <image>`, revision 2) and passes its
// lines on, which this host logs and reads back as a SetupRun. Nothing here waits for the player: the window loop keeps
// presenting. Not built into the CPU tests (they fake the Host).
#pragma once
#include "kernel/disc_swap.h"
#include <memory>
#include <string>

namespace runtime::disc {

// The host for `panel` (fm4recomp --disc-setup; play.sh passes the kit's bin/fm4recomp-setup), reading $FM4_RECOMP_HOME/locations.conf.
std::unique_ptr<Host> MakeSdlHost(const std::string& panel);

}  // namespace runtime::disc
