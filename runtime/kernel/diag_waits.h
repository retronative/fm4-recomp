// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp (diagnostic, FM4_DIAG_WAITS=1): every 10 s, the guest
// threads blocked in a dispatcher wait for more than a second (the objects, their types, how long) and the bytes the guest read from
// files in that window, so a phase where the game stalls (e.g. a race load that only starts after a video) shows what it waits on.
// Behaviour is unchanged; only "[diag waits]" lines are printed.
#pragma once
#include <cstdint>

namespace runtime::diag {

bool WaitsEnabled();
// The calling host thread starts waiting on `count` dispatcher headers (the first four are recorded) as guest thread `thread`.
void WaitBegin(const uint8_t* base, const uint32_t* headers, uint32_t count, uint32_t thread);
void WaitEnd();
// NtReadFile delivered `bytes`.
void ReadBytes(uint64_t bytes);
// A disc file was opened (printed with the vblank, so a phase's reads can be named).
void Opened(const char* path);

}  // namespace runtime::diag
