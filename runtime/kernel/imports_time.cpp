// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_time.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Time imports (M3-T04). The guest system time is the host's wall clock as a Windows FILETIME
// (thread_state.h GuestSystemTime). REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// xboxkrnl_threading.cc KeQuerySystemTime), whose clock may be scaled; this one is not.
#include "clock.h"
#include "imports.h"
#include "thread_state.h"

GUEST_IMPORT(KeQuerySystemTime, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeQuerySystemTime (host wall clock)")
{
    const uint64_t now = runtime::GuestSystemTime();
    runtime::Store32(base, ctx.r3.u32, uint32_t(now >> 32));
    runtime::Store32(base, ctx.r3.u32 + 4, uint32_t(now));
}

GUEST_IMPORT(KeQueryPerformanceFrequency, "REFERENCE_DERIVED: ReCarbon/Unleashed 49,875,000 Hz (clock.h; Xenia uses 50 MHz)")
{
    ctx.r3.u64 = runtime::kTimebaseHz;  // the time base (mftb) frequency
}
