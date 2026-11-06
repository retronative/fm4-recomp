// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/clock.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "clock.h"
#include "imports.h"
#include "thread_state.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace runtime {
namespace {

const auto boot = std::chrono::steady_clock::now();

// The bundle is read by guest threads while it is updated (as on the console): word stores are
// atomic, so a reader sees whole words.
void Put32(uint8_t* base, uint32_t address, uint32_t value)
{
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + address)).store(__builtin_bswap32(value), std::memory_order_relaxed);
}

void Put64(uint8_t* base, uint32_t address, uint64_t value)
{
    Put32(base, address, uint32_t(value >> 32));
    Put32(base, address + 4, uint32_t(value));
}

}  // namespace

uint64_t GuestTimebase()
{
    const auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - boot).count());
    return uint64_t((unsigned __int128)ns * kTimebaseHz / 1000000000u);
}

uint64_t GuestInterruptTime()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - boot).count() / 100);
}

uint32_t GuestUptimeMs()
{
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - boot).count());
}

void WriteTimeStampBundle(uint8_t* base, uint32_t bundle)
{
    Put64(base, bundle, GuestInterruptTime());
    Put64(base, bundle + 8, GuestSystemTime());
    Put32(base, bundle + 16, GuestUptimeMs());
}

void StartTimeStampBundle(uint8_t* base, uint32_t bundle)
{
    WriteTimeStampBundle(base, bundle);
    std::thread([base, bundle] {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            WriteTimeStampBundle(base, bundle);
        }
    }).detach();
}

}  // namespace runtime

// mftb (patch 0015: the translation reads the guest time base here): time base ticks since boot.
uint64_t PpcQueryTimebase() { return runtime::GuestTimebase(); }
