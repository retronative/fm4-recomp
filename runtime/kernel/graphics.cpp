// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The GPU's register page and engine state (graphics.h).
#include "graphics.h"
#include "guest_memory.h"
#include "imports.h"
#include "indirect.h"
#include "threads.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace runtime::gpu {
namespace {
Engines engines;

std::mutex interruptLock;
std::condition_variable interruptWake, interruptDone;
std::deque<uint32_t> pending;                // queued sources
std::atomic<uint32_t> callback{0}, userData{0};
std::atomic<uint64_t> vblanks{0}, delivered{0};
bool started = false;

// The service thread: runs each queued interrupt's callback(source, user data), in order.
void ServeInterrupts(uint8_t* base, PPCContext& initial)
{
    for (;;)
    {
        uint32_t source;
        {
            std::unique_lock guard(interruptLock);
            interruptWake.wait(guard, [] { return !pending.empty(); });
            source = pending.front();
        }
        if (const uint32_t routine = callback.load())
        {
            PPCContext ctx = initial;
            ctx.r3.u64 = source, ctx.r4.u64 = userData.load();
            PpcCallIndirect(ctx, base, routine);
        }
        {
            std::lock_guard guard(interruptLock);
            pending.pop_front();
            delivered++;
        }
        interruptDone.notify_all();
    }
}
}  // namespace

void SetInterruptCallback(uint8_t* base, uint32_t routine, uint32_t data)
{
    userData = data;
    callback = routine;
    std::lock_guard guard(interruptLock);
    if (started)
        return;
    started = true;
    StartServiceThread(base, 2, [base](PPCContext& ctx) { ServeInterrupts(base, ctx); });
    std::thread([] {
        constexpr auto period = std::chrono::nanoseconds(1000000000 / 60);
        auto next = std::chrono::steady_clock::now() + period;
        for (;;)
        {
            std::this_thread::sleep_until(next);
            next += period;
            vblanks++;
            std::lock_guard g(interruptLock);
            if (std::find(pending.begin(), pending.end(), 0u) == pending.end())
                pending.push_back(0);
            interruptWake.notify_one();
        }
    }).detach();
}

void RequestInterrupt(uint32_t source, bool wait)
{
    std::unique_lock guard(interruptLock);
    pending.push_back(source);
    const uint64_t target = delivered + pending.size();
    interruptWake.notify_one();
    if (wait)
        interruptDone.wait(guard, [&] { return delivered >= target; });
}

uint64_t VblankCount() { return vblanks.load(); }
uint64_t InterruptsDelivered() { return delivered.load(); }

void InitializeRegisters(uint8_t* base)
{
    CommitGuestMemory(base, kRegisterBase, kRegisterBytes);
    const uint32_t seeds[][2] = {{0x0F00, 0x08100748}, {0x0F01, 0x0000200E}, {0x194C, 0x000002D0}, {0x1951, 1}, {0x1961, 0x050002D0}};
    for (const auto& s : seeds)
        WriteRegister(base, s[0], s[1]);
}

uint32_t ReadRegister(const uint8_t* base, uint32_t index) { return Load32(base, RegisterAddress(index)); }
void WriteRegister(uint8_t* base, uint32_t index, uint32_t value) { Store32(base, RegisterAddress(index), value); }

Engines& EngineState() { return engines; }

namespace {
const char* gpuStopPath = nullptr;
}  // namespace
void SetGpuStopPath(const char* path) { gpuStopPath = path; }
const char* GpuStopPath() { return gpuStopPath; }

}  // namespace runtime::gpu
