// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/stops.h (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: Strict (ForzaRecomp's PpcStrictStop, patch 0029) is Residual: our patch 0035's
// PpcResidualStop for the accepted residual switch dispatches.
//
// Strict stops: everything the runtime does not implement ends the run with a report and an exit
// code the launcher checks (scripts/run_attempt.sh). Nothing is ever faked as success.
#pragma once
#include <ppc_config.h>
#include <ppc_context.h>

enum class StopCode : int
{
    Import = 3,         // an import was called (none is implemented in M2)
    Residual = 4,       // a residual switch dispatch was reached (PpcResidualStop, patch 0035)
    GuestFault = 5,     // a guest access to unmapped memory (data-import guards included)
    IndirectCall = 6,   // an indirect call to an address with no translated function
    Unimplemented = 7,  // a runtime interface the translation needs (M3)
    EntryReturned = 8,  // the title entry returned to the host
    Setup = 9,          // the runtime could not start (wrong image, no memory, bad arguments)
    Trap = 10,          // a translated trap: a proven switch's impossible default, padding reached
    FirstSwap = 11,     // M3's goal: the command processor executed the first swap packet (VdSwap)
    GpuSelfCheck = 12,  // M4: fm4recomp --renderer self-check passed (runtime/video/renderer.h)
    GpuStranded = 13,   // M4: a GPU submission's completion is unknown: GPU_STOP written, resources kept
    Deadline = 124,     // no stop within the deadline (a hang)
};

[[noreturn]] void Stop(StopCode code, const char* format, ...) __attribute__((format(printf, 2, 3)));
// From a signal handler (the guest fault and trap handlers): the same report, but the GPU is only
// told to stop and its in-flight count reported (no bounded wait: sleeping is not signal-safe).
[[noreturn]] void StopFromSignal(StopCode code, const char* format, ...) __attribute__((format(printf, 2, 3)));
// The renderer's hooks (runtime/video/renderer.cpp; M4): `quiesce` begins the GPU stop and waits,
// bounded, for submissions in flight; `signalReport` only begins it and reports (signal-safe). Null
// (headless, the runtime tests) does nothing. After ForzaRecomp runtime/stops.cpp:97-152.
void SetStopQuiesce(void (*quiesce)(), void (*signalReport)());

// Called by every generated import stub (fm4_imports.cpp).
[[noreturn]] void PpcImportStop(const char* name, PPCContext& ctx);

// Hang report (M3 "a hang report with guest registers"): each guest thread registers its context while
// it runs; the deadline prints every registered thread's lr, ctr, r1 and r3-r6 before stopping (124).
// The values are the context's memory, so a register the compiled code holds elsewhere may be older.
constexpr size_t kMaxGuestContexts = 64;
void RegisterGuestContext(const PPCContext* ctx, uint32_t kthread);
void UnregisterGuestContext(const PPCContext* ctx);
void ReportGuestContexts();
