// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4_DIAG_WAITS (diag_waits.h). Diagnostic only.
#include "diag_waits.h"
#include "graphics.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace runtime::diag {
namespace {
using Clock = std::chrono::steady_clock;
struct Waiting
{
    uint32_t thread = 0, count = 0, headers[4] = {}, types[4] = {};
    Clock::time_point since;
};
std::mutex lock;
std::map<std::thread::id, Waiting> waiting;
std::atomic<uint64_t> readBytes{0};

void Reporter()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const auto now = Clock::now();
        std::lock_guard guard(lock);
        std::fprintf(stderr, "[diag waits] vblank %llu: file reads %llu KB in the last 10 s; threads blocked over 1 s:\n",
                     (unsigned long long)runtime::gpu::VblankCount(), (unsigned long long)(readBytes.exchange(0) / 1024));
        for (const auto& [id, w] : waiting)
        {
            const double seconds = std::chrono::duration<double>(now - w.since).count();
            if (seconds < 1.0)
                continue;
            std::fprintf(stderr, "[diag waits]   guest thread %08X for %.1f s on %u object(s):", w.thread, seconds, w.count);
            for (uint32_t i = 0; i < w.count && i < 4; i++)
                std::fprintf(stderr, " %08X (type %u)", w.headers[i], w.types[i]);
            std::fprintf(stderr, "\n");
        }
    }
}
}  // namespace

bool WaitsEnabled()
{
    static const bool on = [] {
        const char* v = std::getenv("FM4_DIAG_WAITS");
        const bool enabled = v != nullptr && !std::strcmp(v, "1");
        if (enabled)
        {
            std::fprintf(stderr, "[diag waits] FM4_DIAG_WAITS=1: blocked guest threads and file reads every 10 s (diagnostic)\n");
            std::thread(Reporter).detach();
        }
        return enabled;
    }();
    return on;
}

void WaitBegin(const uint8_t* base, const uint32_t* headers, uint32_t count, uint32_t thread)
{
    if (!WaitsEnabled())
        return;
    Waiting w;
    w.thread = thread, w.count = count, w.since = Clock::now();
    for (uint32_t i = 0; i < count && i < 4; i++)
        w.headers[i] = headers[i], w.types[i] = base[headers[i]];
    std::lock_guard guard(lock);
    waiting[std::this_thread::get_id()] = w;
}

void WaitEnd()
{
    if (!WaitsEnabled())
        return;
    std::lock_guard guard(lock);
    waiting.erase(std::this_thread::get_id());
}

void ReadBytes(uint64_t bytes)
{
    if (WaitsEnabled())
        readBytes += bytes;
}

void Opened(const char* path)
{
    if (WaitsEnabled())
        std::fprintf(stderr, "[diag waits] vblank %llu: opened %s\n", (unsigned long long)runtime::gpu::VblankCount(), path);
}

}  // namespace runtime::diag
