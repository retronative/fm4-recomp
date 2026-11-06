// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/threads.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Guest threads on host threads (M3-T05). REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// xboxkrnl_threading.cc ExCreateThread/ExTerminateThread/NtResumeThread, xthread.cc Execute/Exit),
// re-derived. Not reproduced: Xenia's mandatory 10 ms start delay (a workaround for one title) and
// APC delivery (no APC is queued anywhere yet; queuing one stops).
#pragma once
#include <cstdint>
#include <functional>

struct PPCContext;

namespace runtime {

struct TlsInfo;

// The title process (set once, before any thread is created).
void SetTitleProcess(uint32_t process, const TlsInfo& tls, uint32_t defaultStackSize);

// Creates a guest thread (suspended if `flags` has CREATE_SUSPENDED, 0x1); returns its KTHREAD.
uint32_t CreateGuestThread(uint8_t* base, uint32_t stackSize, uint32_t xapiStartup, uint32_t start, uint32_t context,
                           uint32_t flags, uint32_t& threadId);
// Decrements a thread's suspend count and starts it at zero; returns the previous count.
uint32_t ResumeGuestThread(uint8_t* base, uint32_t kthread);
// Increments a thread's suspend count (NT NtSuspendThread; INFERRED): for a thread not yet past
// its start (it stays waiting there), or for the calling thread itself (`current`), which then
// blocks until resumed. `previous` receives the count before. Suspending another thread that is
// running or has ended is not implemented and stops (it would need suspension points). Returns
// 0, or STATUS_SUSPEND_COUNT_EXCEEDED at the limit (127).
uint32_t SuspendGuestThread(uint8_t* base, uint32_t kthread, uint32_t current, uint32_t& previous);

// Runs `body` on a new host thread that is a guest thread of the title process (its own KTHREAD,
// TLS and stack, on processor `cpu`), for runtime services that call guest code (the GPU's
// interrupt callbacks). `body` gets the thread's initial context and should not return.
void StartServiceThread(uint8_t* base, uint8_t cpu, std::function<void(PPCContext&)> body);

// Unwinds the calling guest thread to its host entry (ExTerminateThread never returns).
[[noreturn]] void ExitCurrentGuestThread(uint32_t exitCode);

namespace kthread_state { constexpr uint32_t kSuspendCount = 0xBC, kTerminated = 0xBE, kExitStatus = 0x140; }

}  // namespace runtime
