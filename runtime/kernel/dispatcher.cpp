// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/dispatcher.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "dispatcher.h"
#include "imports.h"
#include "thread_state.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace runtime {
namespace {

std::mutex lock;
std::condition_variable changed;
constexpr uint8_t kThreadObject = 6;  // a KTHREAD's dispatcher header type

}  // namespace

int32_t SetEvent(uint8_t* base, uint32_t header)
{
    std::lock_guard guard(lock);
    const int32_t previous = int32_t(Load32(base, header + dispatch::kSignalState));
    Store32(base, header + dispatch::kSignalState, 1);
    changed.notify_all();
    return previous;
}

int32_t ResetEvent(uint8_t* base, uint32_t header)
{
    std::lock_guard guard(lock);
    const int32_t previous = int32_t(Load32(base, header + dispatch::kSignalState));
    Store32(base, header + dispatch::kSignalState, 0);
    return previous;
}

void WaitForEvent(uint8_t* base, uint32_t header) { WaitForObject(base, header, nullptr); }

namespace {

void CheckWaitable(const uint8_t* base, uint32_t header)
{
    const uint8_t type = base[header];
    if (type != dispatch::kNotificationEvent && type != dispatch::kSynchronizationEvent && type != kThreadObject &&
        type != dispatch::kSemaphore && type != dispatch::kNotificationTimer && type != dispatch::kSynchronizationTimer &&
        type != dispatch::kMutant)
        Stop(StopCode::Unimplemented, "a wait on dispatcher object type %u at %08X is not implemented", type, header);
}

void CheckWaitable(const uint8_t* base, uint32_t header, uint32_t thread)
{
    CheckWaitable(base, header);
    if (base[header] == dispatch::kMutant && thread == 0)
        Stop(StopCode::Unimplemented, "a wait on the mutant at %08X by a thread the runtime cannot name", header);
}

bool Signalled(const uint8_t* base, uint32_t header, uint32_t thread)
{
    const int32_t state = int32_t(Load32(base, header + dispatch::kSignalState));
    if (base[header] == dispatch::kMutant)
        return state > 0 || Load32(base, header + kMutantOwner) == thread;
    return state != 0;
}

// What a satisfied wait does to its object: a synchronization event or timer is reset, a
// semaphore loses one unit of its count; notification events and timers and threads stay signalled.
void Consume(uint8_t* base, uint32_t header, uint32_t thread)
{
    if (base[header] == dispatch::kMutant)
    {
        Store32(base, header + dispatch::kSignalState, Load32(base, header + dispatch::kSignalState) - 1);
        Store32(base, header + kMutantOwner, thread);
    }
    else if (base[header] == dispatch::kSynchronizationEvent || base[header] == dispatch::kSynchronizationTimer)
        Store32(base, header + dispatch::kSignalState, 0);
    else if (base[header] == dispatch::kSemaphore)
        Store32(base, header + dispatch::kSignalState, Load32(base, header + dispatch::kSignalState) - 1);
}

// Blocks (lock held) until `ready` holds or the timeout passes: 100 ns units, negative relative,
// positive an absolute system time, null infinite. A timeout more than about 29,000 years away
// (beyond what the host clock can add) is infinite.
template <typename Ready>
bool Block(std::unique_lock<std::mutex>& guard, const int64_t* timeout, Ready ready)
{
    uint64_t ticks = 0;
    if (timeout != nullptr)
    {
        const uint64_t now = GuestSystemTime(), value = uint64_t(*timeout);
        ticks = *timeout < 0 ? 0 - value : value > now ? value - now : 0;
    }
    constexpr uint64_t kForever = uint64_t(INT64_MAX) / 100 / 2;
    if (timeout == nullptr || ticks > kForever)
    {
        changed.wait(guard, ready);
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(int64_t(ticks) * 100);
    return changed.wait_until(guard, deadline, ready);
}

bool WaitLocked(std::unique_lock<std::mutex>& guard, uint8_t* base, uint32_t header, const int64_t* timeout, uint32_t thread)
{
    if (!Block(guard, timeout, [&] { return Signalled(base, header, thread); }))
        return false;
    Consume(base, header, thread);
    return true;
}

}  // namespace

bool WaitForObject(uint8_t* base, uint32_t header, const int64_t* timeout, uint32_t thread)
{
    CheckWaitable(base, header, thread);
    std::unique_lock guard(lock);
    return WaitLocked(guard, base, header, timeout, thread);
}

uint32_t SignalAndWaitForObject(uint8_t* base, uint32_t signal, uint32_t header, const int64_t* timeout, uint32_t thread)
{
    CheckWaitable(base, header, thread);
    const uint8_t type = base[signal];
    if (type != dispatch::kNotificationEvent && type != dispatch::kSynchronizationEvent && type != dispatch::kSemaphore)
        Stop(StopCode::Unimplemented, "signal-and-wait signalling dispatcher object type %u at %08X is not implemented", type, signal);
    std::unique_lock guard(lock);
    const uint32_t state = Load32(base, signal + dispatch::kSignalState);
    if (type == dispatch::kSemaphore)
    {
        if (int64_t(int32_t(state)) + 1 > int32_t(Load32(base, signal + dispatch::kSemaphoreLimit)))
            return wait::kSemaphoreLimitExceeded;  // as NT (INFERRED; Xenia's host call fails): no signal, no wait
        Store32(base, signal + dispatch::kSignalState, state + 1);
    }
    else
        Store32(base, signal + dispatch::kSignalState, 1);
    changed.notify_all();
    return WaitLocked(guard, base, header, timeout, thread) ? wait::kSuccess : wait::kTimeout;
}

uint32_t WaitForObjects(uint8_t* base, const uint32_t* headers, uint32_t count, bool all, const int64_t* timeout, uint32_t thread)
{
    for (uint32_t i = 0; i < count; i++)
    {
        CheckWaitable(base, headers[i], thread);
        for (uint32_t j = 0; j < i && all; j++)
            if (headers[j] == headers[i])
                Stop(StopCode::Unimplemented, "a wait for all of several objects lists the object at %08X twice", headers[i]);
    }
    std::unique_lock guard(lock);
    uint32_t index = 0;
    const auto ready = [&] {
        for (uint32_t i = 0; i < count; i++)
        {
            if (Signalled(base, headers[i], thread) != all)
            {
                index = i;
                return !all;  // any: the first signalled object; all: one unsignalled object blocks
            }
        }
        return all;
    };
    if (!Block(guard, timeout, ready))
        return wait::kTimeout;
    if (!all)
    {
        Consume(base, headers[index], thread);
        return index;  // STATUS_WAIT_0 + index
    }
    for (uint32_t i = 0; i < count; i++)
        Consume(base, headers[i], thread);
    return wait::kSuccess;
}

bool ReleaseMutant(uint8_t* base, uint32_t header, uint32_t thread, int32_t& previous)
{
    std::lock_guard guard(lock);
    previous = int32_t(Load32(base, header + dispatch::kSignalState));
    if (previous > 0 || Load32(base, header + kMutantOwner) != thread)
        return false;
    Store32(base, header + dispatch::kSignalState, uint32_t(previous + 1));
    if (previous + 1 == 1)
        Store32(base, header + kMutantOwner, 0);
    changed.notify_all();
    return true;
}

bool ReleaseSemaphore(uint8_t* base, uint32_t header, int32_t count, int32_t& previous)
{
    std::lock_guard guard(lock);
    previous = int32_t(Load32(base, header + dispatch::kSignalState));
    if (count <= 0 || int64_t(previous) + count > int32_t(Load32(base, header + dispatch::kSemaphoreLimit)))
        return false;
    Store32(base, header + dispatch::kSignalState, uint32_t(previous + count));
    changed.notify_all();
    return true;
}

}  // namespace runtime
