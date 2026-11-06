// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/arena.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "arena.h"
#include "guest_memory.h"
#include "stops.h"
#include <cstring>
#include <mutex>
#include <vector>
#include <unistd.h>

namespace runtime {
namespace {

std::mutex lock;
uint32_t arenaNext = kArenaBase, arenaCommitted = kArenaBase;
uint32_t stacksNext = kStacksBase;
std::vector<GuestStack> stacks;  // every stack allocated (never freed)

}  // namespace

uint32_t ArenaAlloc(uint8_t* base, uint32_t size, uint32_t align)
{
    std::lock_guard guard(lock);
    const uint32_t start = (arenaNext + align - 1) & ~(align - 1);
    if (size > kArenaEnd - start)
        Stop(StopCode::Setup, "the kernel arena is full (%u bytes requested)", size);
    arenaNext = start + size;
    if (arenaNext > arenaCommitted)
    {
        const uint32_t page = uint32_t(getpagesize());
        const uint32_t end = (arenaNext + page - 1) & ~(page - 1);
        CommitGuestMemory(base, arenaCommitted, end - arenaCommitted);
        arenaCommitted = end;
    }
    std::memset(base + start, 0, size);
    return start;
}

GuestStack AllocateStack(uint8_t* base, uint32_t size)
{
    std::lock_guard guard(lock);
    const uint32_t page = uint32_t(getpagesize());
    const GuestStack stack = PlaceStack(stacksNext, size, page);
    if (stack.base + page > kStacksEnd || stack.base < stack.limit)
        Stop(StopCode::Setup, "no room for a guest stack of %u bytes", size);
    CommitGuestMemory(base, stack.limit, stack.base - stack.limit);  // the guards stay unmapped
    stacksNext = stack.base + page;
    stacks.push_back(stack);
    return stack;
}

bool InGuestStack(uint32_t address)
{
    std::lock_guard guard(lock);
    for (const GuestStack& s : stacks)
        if (address >= s.limit && address < s.base)
            return true;
    return false;
}

}  // namespace runtime
