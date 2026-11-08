// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/arena.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: ReleaseStack (MmDeleteKernelStack).
//
// Guest memory the runtime owns: the kernel arena (objects the kernel would allocate: PCRs,
// KTHREADs, TLS blocks, the process block, the XEX header copy) and thread stacks. Both live in
// runtime-chosen ranges below the data-import guards (layout.h); nothing here is a title address.
#pragma once
#include <cstddef>
#include <cstdint>

namespace runtime {

constexpr uint32_t kArenaBase = 0x7E000000u, kArenaEnd = 0x7F000000u;
constexpr uint32_t kStacksBase = 0x70000000u, kStacksEnd = 0x7E000000u;

// Zeroed, committed guest memory from the kernel arena (16-byte aligned or more); stops when full.
uint32_t ArenaAlloc(uint8_t* base, uint32_t size, uint32_t align = 16);

// A guest stack of at least `size` bytes with an unmapped guard page below and above it.
struct GuestStack { uint32_t limit, base; };  // limit = lowest usable byte, base = one past the top
GuestStack AllocateStack(uint8_t* base, uint32_t size);

// Releases a stack AllocateStack returned, given its top (`base`) and lowest usable byte (`limit`): its pages are
// discarded and become inaccessible (a later access faults); the range is not reused. False if no live stack matches.
bool ReleaseStack(uint8_t* base, uint32_t top, uint32_t limit);

// Whether `address` is in a guest stack's usable pages (read/write) rather than a guard or an
// unused part of the stack range.
bool InGuestStack(uint32_t address);

// Pure placement of the n-th stack (unit-tested): guard, stack, guard, consecutive.
constexpr GuestStack PlaceStack(uint32_t next, uint32_t size, uint32_t page)
{
    const uint32_t rounded = (size + page - 1) & ~(page - 1);
    return {next + page, next + page + rounded};
}

}  // namespace runtime
