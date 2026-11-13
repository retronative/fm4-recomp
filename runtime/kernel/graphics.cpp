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
#include <cstdio>
#include <cstdlib>
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

// FM4 Recomp (FM4_UNLOCKED_FPS, graphics.h): the early-vblank request, its minimum interval, and how many vblanks came early.
std::mutex vblankLock;
std::condition_variable vblankWake;
bool earlyVblank = false;
std::atomic<uint64_t> earlyVblanks{0};
std::chrono::nanoseconds UnlockedMinInterval()
{
    static const std::chrono::nanoseconds interval = [] {
        const char* v = std::getenv("FM4_UNLOCKED_FPS_MIN_US");
        const unsigned long us = v != nullptr ? std::strtoul(v, nullptr, 10) : 8333;
        return std::chrono::microseconds(us >= 1000 && us <= 16667 ? us : 8333);
    }();
    return interval;
}

// FM4 Recomp (diagnostic, FM4_DIAG_INTERRUPTS=1): how long each queued interrupt waits before its callback starts and how long the
// guest callback runs, by source, summed and reported every 300 PM4 interrupts. FM4's callback acknowledges a PM4 interrupt
// itself (it clears its CPU's bit in the word the command stream's WAIT_REG_MEM polls), so these are the guest side of a drain.
bool DiagInterrupts()
{
    static const bool on = [] { const char* v = std::getenv("FM4_DIAG_INTERRUPTS"); return v != nullptr && v[0] == '1' && v[1] == '\0'; }();
    return on;
}
uint64_t NowNs() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
struct InterruptTally { uint64_t n = 0, queueNs = 0, runNs = 0, maxRunNs = 0, maxQueueNs = 0; };
InterruptTally tally[2];
std::deque<uint64_t> queuedAt;  // FM4_DIAG_INTERRUPTS: when each pending entry was queued (under interruptLock)

// The service thread: runs each queued interrupt's callback(source, user data), in order.
void ServeInterrupts(uint8_t* base, PPCContext& initial)
{
    for (;;)
    {
        uint32_t source;
        uint64_t queued = 0;
        {
            std::unique_lock guard(interruptLock);
            interruptWake.wait(guard, [] { return !pending.empty(); });
            source = pending.front();
            queued = DiagInterrupts() && !queuedAt.empty() ? queuedAt.front() : 0;
        }
        const uint64_t begun = DiagInterrupts() ? NowNs() : 0;
        if (const uint32_t routine = callback.load())
        {
            PPCContext ctx = initial;
            ctx.r3.u64 = source, ctx.r4.u64 = userData.load();
            PpcCallIndirect(ctx, base, routine);
        }
        {
            std::lock_guard guard(interruptLock);
            pending.pop_front();
            if (DiagInterrupts() && !queuedAt.empty())
            {
                queuedAt.pop_front();
                InterruptTally& t = tally[source == 0 ? 0 : 1];
                const uint64_t run = NowNs() - begun, wait = queued != 0 && begun > queued ? begun - queued : 0;
                t.n++, t.runNs += run, t.queueNs += wait, t.maxRunNs = std::max(t.maxRunNs, run), t.maxQueueNs = std::max(t.maxQueueNs, wait);
                if (source != 0 && tally[1].n % 300 == 0)
                {
                    std::fprintf(stderr, "[interrupts] PM4: %llu, queue %.3f ms mean (max %.3f), callback %.3f ms mean (max %.3f); vblank: %llu, queue %.3f, "
                                         "callback %.3f ms mean (max %.3f)\n", (unsigned long long)tally[1].n, tally[1].queueNs / 1e6 / tally[1].n,
                                 tally[1].maxQueueNs / 1e6, tally[1].runNs / 1e6 / tally[1].n, tally[1].maxRunNs / 1e6, (unsigned long long)tally[0].n,
                                 tally[0].n ? tally[0].queueNs / 1e6 / tally[0].n : 0.0, tally[0].n ? tally[0].runNs / 1e6 / tally[0].n : 0.0,
                                 tally[0].maxRunNs / 1e6);
                    tally[0] = tally[1] = {};
                }
            }
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
        using clock = std::chrono::steady_clock;
        constexpr auto period = std::chrono::nanoseconds(1000000000 / 60);
        auto next = clock::now() + period, last = clock::now();
        for (;;)
        {
            if (!UnlockedFrameRate())
                std::this_thread::sleep_until(next), next += period;
            else  // FM4_UNLOCKED_FPS: the 60 Hz deadline, or an early request once the minimum interval since the last vblank passed
            {
                std::unique_lock g(vblankLock);
                vblankWake.wait_until(g, next, [] { return earlyVblank; });
                auto now = clock::now();
                if (now < next && now < last + UnlockedMinInterval())
                    vblankWake.wait_until(g, std::min(next, last + UnlockedMinInterval()), [] { return false; }), now = clock::now();
                const bool early = now < next;
                earlyVblank = false;
                if (early)
                    earlyVblanks++;
                next = now + period, last = now;
            }
            vblanks++;
            if (UnlockedFrameRate() && vblanks % 600 == 0)
                std::fprintf(stderr, "[vblank] %llu vblanks, %llu early (FM4_UNLOCKED_FPS)\n", (unsigned long long)vblanks.load(),
                             (unsigned long long)earlyVblanks.load());
            std::lock_guard g(interruptLock);
            if (std::find(pending.begin(), pending.end(), 0u) == pending.end())
            {
                pending.push_back(0);
                if (DiagInterrupts())
                    queuedAt.push_back(NowNs());
            }
            interruptWake.notify_one();
        }
    }).detach();
}

void RequestInterrupt(uint32_t source, bool wait, void (*poll)())
{
    std::unique_lock guard(interruptLock);
    pending.push_back(source);
    if (DiagInterrupts())
        queuedAt.push_back(NowNs());
    const uint64_t target = delivered + pending.size();
    interruptWake.notify_one();
    if (wait && poll == nullptr)
        interruptDone.wait(guard, [&] { return delivered >= target; });
    else if (wait)  // FM4 Recomp (FM4_GUEST_MEMORY_GPU): runtime/video/gpu_read_guard.h G5
        while (!interruptDone.wait_for(guard, std::chrono::microseconds(50), [&] { return delivered >= target; }))
        {
            guard.unlock();
            poll();
            guard.lock();
        }
}

uint64_t VblankCount() { return vblanks.load(); }

bool UnlockedFrameRate()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_UNLOCKED_FPS");
        const bool enabled = v != nullptr && v[0] == '1' && v[1] == '\0';
        if (enabled)
            std::fprintf(stderr, "[gpu] unlocked frame rate on (FM4_UNLOCKED_FPS=1): a swap waiting for its vblank gets it at once, at most every %lld us\n",
                         (long long)std::chrono::duration_cast<std::chrono::microseconds>(UnlockedMinInterval()).count());
        return enabled;
    }();
    return on;
}

void RequestEarlyVblank()
{
    if (!UnlockedFrameRate())
        return;
    std::lock_guard g(vblankLock);
    earlyVblank = true;
    vblankWake.notify_one();
}

uint64_t EarlyVblanks() { return earlyVblanks.load(); }
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
std::mutex rampLock;
gamma::Ramp ramp = gamma::DefaultRamp();
uint64_t rampCpWrites = 0, rampCpuWrites = 0;
}  // namespace

void OnGammaRampRegisterWritten(uint8_t* base, uint32_t index, uint32_t value, LutWriter writer)
{
    if (!gamma::IsStateRegister(index))
        return;
    std::lock_guard guard(rampLock);
    (writer == LutWriter::Cpu ? rampCpuWrites : rampCpWrites)++;
    const uint32_t mode = ReadRegister(base, gamma::reg::kRwMode), enable = ReadRegister(base, gamma::reg::kWriteEnMask);
    const uint32_t before = ReadRegister(base, gamma::reg::kRwIndex);
    uint32_t rwIndex = before;
    if (gamma::OnRegisterWritten(ramp, index, value, mode, enable, rwIndex) == gamma::Write::ModeMismatch)
        Stop(StopCode::Unimplemented, "GPU: DC_LUT register %X written with %08X while DC_LUT_RW_MODE is %08X (Xenia asserts "
             "against it, command_processor.cc:477/513/548; not modelled)", index, value, mode);
    if (rwIndex != before)
        WriteRegister(base, gamma::reg::kRwIndex, rwIndex);
}

GammaRampView CurrentGammaRamp()
{
    std::lock_guard guard(rampLock);
    return {ramp, rampCpWrites, rampCpuWrites};
}

namespace {
const char* gpuStopPath = nullptr;
}  // namespace
void SetGpuStopPath(const char* path) { gpuStopPath = path; }
const char* GpuStopPath() { return gpuStopPath; }

}  // namespace runtime::gpu
