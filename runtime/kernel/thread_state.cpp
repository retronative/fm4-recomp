// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/thread_state.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "thread_state.h"
#include "arena.h"
#include "guest_memory.h"
#include <chrono>
#include <cstring>
#include <mutex>

namespace runtime {
namespace {

void Put8(uint8_t* base, uint32_t address, uint8_t value) { base[address] = value; }
void Put16(uint8_t* base, uint32_t address, uint16_t value)
{
    const uint16_t swapped = __builtin_bswap16(value);
    std::memcpy(base + address, &swapped, 2);
}
void Put32(uint8_t* base, uint32_t address, uint32_t value) { StoreBigEndian32(base, address, value); }
uint32_t Load32Be(const uint8_t* base, uint32_t address)
{
    uint32_t v;
    std::memcpy(&v, base + address, 4);
    return __builtin_bswap32(v);
}
void Put64(uint8_t* base, uint32_t address, uint64_t value)
{
    Put32(base, address, uint32_t(value >> 32));
    Put32(base, address + 4, uint32_t(value));
}

std::mutex threadsLock;
std::vector<ThreadState> threads;

}  // namespace

std::vector<uint32_t> AllThreads()
{
    std::lock_guard guard(threadsLock);
    std::vector<uint32_t> kthreads;
    for (const auto& t : threads)
        kthreads.push_back(t.kthread);
    return kthreads;
}

bool FindThread(uint32_t kthread, ThreadState& state)
{
    std::lock_guard guard(threadsLock);
    for (const auto& t : threads)
        if (t.kthread == kthread)
        {
            state = t;
            return true;
        }
    return false;
}

uint64_t GuestSystemTime()
{
    constexpr uint64_t kUnixEpochAsFiletime = 116444736000000000ull;  // 1970-01-01 in 100 ns since 1601
    const auto since = std::chrono::system_clock::now().time_since_epoch();
    return kUnixEpochAsFiletime + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count() / 100);
}

uint32_t CreateProcessBlock(uint8_t* base, const TlsInfo& tls)
{
    const uint32_t p = ArenaAlloc(base, kProcessBlockSize);
    Put32(base, p + 0x0C, 0x0000007F);
    Put32(base, p + 0x10, 0x001F0000);
    Put8(base, p + 0x1B, 0x06);
    Put32(base, p + 0x1C, 16 * 1024);  // kernel stack size
    Put32(base, p + process::kTlsDataSize, tls.dataSize);
    Put32(base, p + process::kTlsRawDataSize, tls.rawDataSize);
    Put16(base, p + process::kTlsSlotSize, uint16_t(tls.slotCount * 4));
    Put8(base, p + process::kProcessType, kProcessTypeTitle);
    return p;
}

ThreadState CreateThreadState(uint8_t* base, const TlsInfo& tls, uint32_t process, uint32_t stackSize,
                              uint32_t threadId, uint32_t startAddress, uint32_t creationFlags, uint8_t cpu)
{
    ThreadState t{};
    const GuestStack stack = AllocateStack(base, stackSize);
    t.stackLimit = stack.limit;
    t.stackBase = stack.base;

    // TLS: the static data (the executable's template, zero beyond it), then one word per slot.
    t.tls = ArenaAlloc(base, tls.dataSize + tls.slotCount * 4);
    std::memcpy(base + t.tls, base + tls.rawDataAddress, tls.rawDataSize);

    const uint32_t k = t.kthread = ArenaAlloc(base, kKthreadSize);
    Put8(base, k + 0x00, 6);  // dispatcher header type: thread
    Put8(base, k + 0xBC, (creationFlags & 1) ? 1 : 0);  // suspend count (CREATE_SUSPENDED)
    for (uint32_t list : {0x10u, 0x74u, 0x7Cu, 0x144u, 0x154u})  // empty list heads
    {
        Put32(base, k + list, k + list);
        Put32(base, k + list + 4, k + list);
    }
    Put32(base, k + 0x40, k + 0x018 + 8);
    Put32(base, k + 0x44, k + 0x018 + 8);
    Put32(base, k + 0x48, k);
    Put32(base, k + 0x4C, k + 0x018);
    Put16(base, k + 0x54, 0x102);
    Put16(base, k + 0x56, 1);
    Put32(base, k + kthread::kStackBase, t.stackBase);
    Put32(base, k + kthread::kStackLimit, t.stackLimit);
    Put32(base, k + 0x64, t.stackBase - 0xF0);  // kernel stack (Xenia kThreadKernelStackSize)
    Put32(base, k + kthread::kTls, t.tls);
    Put32(base, k + kthread::kProcess, process);
    Put32(base, process + 0x14, Load32Be(base, process + 0x14) + 1);  // the process's thread count (Xenia)
    Put8(base, k + 0x8B, 1);
    Put32(base, k + 0x9C, 0xFDFFD7FF);
    Put8(base, k + kthread::kCurrentCpu, cpu);
    Put32(base, k + 0xD0, t.stackBase);
    Put64(base, k + 0x130, GuestSystemTime());  // create time
    Put32(base, k + kthread::kThreadId, threadId);
    Put32(base, k + kthread::kStartAddress, startAddress);
    Put32(base, k + kthread::kCreationFlags, creationFlags);
    Put32(base, k + 0x17C, 1);

    const uint32_t p = t.pcr = ArenaAlloc(base, kPcrSize);
    Put32(base, p + pcr::kTls, t.tls);
    Put32(base, p + pcr::kSelf, p);
    Put32(base, p + pcr::kStackBase, t.stackBase);
    Put32(base, p + pcr::kStackEnd, t.stackLimit);
    Put32(base, p + pcr::kCurrentThread, k);
    Put8(base, p + pcr::kCurrentCpu, cpu);
    std::lock_guard guard(threadsLock);
    threads.push_back(t);
    return t;
}

}  // namespace runtime
