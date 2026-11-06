// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_timer.cpp (GPL-3.0-or-later, same owner),
// reviewed and re-tested here (tests/runtime/test_runtime.cpp). FM4 Recomp changes: STATUS_INVALID_PARAMETER written
// as an expression; citations with Xenia line numbers (xboxkrnl_threading.cc:798-877).
//
// Kernel timers. REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// kernel/xboxkrnl/xboxkrnl_threading.cc NtCreateTimer/NtSetTimerEx/NtCancelTimer, xtimer.cc) and
// NT's documented timer behaviour (INFERRED where noted), re-derived. A timer is a kernel object
// whose body is a dispatcher header (type 8 notification, 9 synchronization: dispatcher.h), so
// waits on it are ordinary dispatcher waits. One host thread signals timers when they are due.
// Not modelled (they stop): named timers, APC routines (no APC delivery), the eighth argument.
#include "arena.h"
#include "thread_state.h"
#include "dispatcher.h"
#include "imports.h"
#include "objects.h"
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace runtime {
namespace {

using Clock = std::chrono::steady_clock;
struct Timer { Clock::time_point due; uint32_t periodMs; bool armed; };
std::mutex timersLock;
std::condition_variable timersChanged;
std::map<uint32_t, Timer> timers;  // by body (dispatcher header)
uint8_t* timerBase = nullptr;
bool timerThreadStarted = false;

// Signals each armed timer when it is due; a periodic one is re-armed one period later.
void TimerThread()
{
    std::unique_lock guard(timersLock);
    for (;;)
    {
        auto next = Clock::time_point::max();
        for (auto& [body, timer] : timers)
        {
            if (!timer.armed)
                continue;
            if (timer.due <= Clock::now())
            {
                SetEvent(timerBase, body);
                if (timer.periodMs)
                    timer.due += std::chrono::milliseconds(timer.periodMs);
                else
                    timer.armed = false;
            }
            if (timer.armed)
                next = std::min(next, timer.due);
        }
        if (next == Clock::time_point::max())
            timersChanged.wait(guard);
        else
            timersChanged.wait_until(guard, next);
    }
}

// A due time (100 ns units: negative relative, positive an absolute system time) as a host time.
Clock::time_point DueTime(int64_t due)
{
    const uint64_t now = GuestSystemTime();
    const uint64_t ticks = due < 0 ? uint64_t(0) - uint64_t(due) : uint64_t(due) > now ? uint64_t(due) - now : 0;
    return Clock::now() + std::chrono::nanoseconds(int64_t(std::min<uint64_t>(ticks, uint64_t(INT64_MAX) / 200)) * 100);
}

}  // namespace

uint32_t ArmedTimerCountForTests()
{
    std::lock_guard guard(timersLock);
    uint32_t count = 0;
    for (const auto& [body, timer] : timers)
        count += timer.armed;
    return count;
}

}  // namespace runtime

GUEST_IMPORT(NtCreateTimer, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtCreateTimer, xtimer.cc Initialize")
{
    // (handle*, attributes, type 0 notification / 1 synchronization): an unsignalled, unarmed
    // timer. Another type is STATUS_INVALID_PARAMETER (NT; Xenia does not check: INFERRED); a name
    // is not modelled and stops.
    using namespace runtime;
    const uint32_t handleOut = ctx.r3.u32, attributes = ctx.r4.u32, type = ctx.r5.u32;
    if (!ObjectName(base, attributes).empty())
        Stop(StopCode::Unimplemented, "NtCreateTimer: named timers are not implemented");
    if (type > 1)
    {
        ctx.r3.u64 = (3u << 30) | 0xD;  // STATUS_INVALID_PARAMETER
        return;
    }
    const uint32_t body = ArenaAlloc(base, 0x28);  // KTIMER: a dispatcher header first
    base[body] = type ? dispatch::kSynchronizationTimer : dispatch::kNotificationTimer;
    Store32(base, body + dispatch::kSignalState, 0);
    {
        std::lock_guard guard(timersLock);
        timers[body] = {Clock::time_point::max(), 0, false};
        if (!timerThreadStarted)
        {
            timerBase = base;
            timerThreadStarted = true;
            std::thread(TimerThread).detach();
        }
    }
    const uint32_t handle = CreateObject(ObjectType::Timer, body, "");
    if (handleOut)
        Store32(base, handleOut, handle);
    ctx.r3.u64 = status::kSuccess;
}

GUEST_IMPORT(NtSetTimerEx, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtSetTimerEx, xtimer.cc SetTimer; signal reset on set from NT (INFERRED)")
{
    // (handle, due time*, APC routine, mode, APC argument, resume, period ms, unknown): arms the
    // timer: it is signalled at the due time, then every period if one is given; setting resets
    // its signal state (NT KeSetTimerEx). Resume (waking a sleeping system) has no effect here. An
    // APC routine or a nonzero last argument (Xenia asserts 0) is not modelled and stops.
    using namespace runtime;
    const uint32_t handle = ctx.r3.u32, dueAddress = ctx.r4.u32, routine = ctx.r5.u32, period = Argument(ctx, base, 6),
                   last = Argument(ctx, base, 7);
    if (routine || last)
        Stop(StopCode::Unimplemented, "NtSetTimerEx with an APC routine (%08X) or last argument %08X is not implemented", routine, last);
    const auto object = LookupHandle(handle);
    if (!object || object->type != ObjectType::Timer)
    {
        ctx.r3.u64 = status::kInvalidHandle;
        return;
    }
    const int64_t due = int64_t(uint64_t(Load32(base, dueAddress)) << 32 | Load32(base, dueAddress + 4));
    {
        std::lock_guard guard(timersLock);
        ResetEvent(base, object->header);
        timers[object->header] = {DueTime(due), period, true};
    }
    timersChanged.notify_all();
    ctx.r3.u64 = status::kSuccess;
}

GUEST_IMPORT(NtCancelTimer, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtCancelTimer; current state from NT (INFERRED)")
{
    // (handle, current state*): disarms the timer; its signal state is kept, and reported (NT;
    // Xenia writes 0).
    using namespace runtime;
    const uint32_t handle = ctx.r3.u32, stateOut = ctx.r4.u32;
    const auto object = LookupHandle(handle);
    if (!object || object->type != ObjectType::Timer)
    {
        ctx.r3.u64 = status::kInvalidHandle;
        return;
    }
    {
        std::lock_guard guard(timersLock);
        timers[object->header].armed = false;
    }
    if (stateOut)
        Store32(base, stateOut, Load32(base, object->header + dispatch::kSignalState) ? 1 : 0);
    ctx.r3.u64 = status::kSuccess;
}
