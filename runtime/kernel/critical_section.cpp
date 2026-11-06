// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/critical_section.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Critical sections (M3-T03). 28 bytes: a dispatcher header (an auto-reset event the waiters
// block on), then lock count (+0x10), recursion count (+0x14) and owning KTHREAD (+0x18).
// REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f: xboxkrnl_rtl.cc
// X_RTL_CRITICAL_SECTION and RtlInitializeCriticalSection[AndSpinCount], RtlEnter-, TryEnter-,
// LeaveCriticalSection), re-derived. By design every guest-visible field is
// big-endian, the lock count included (Xenia keeps it in host order; XDK code reads the recursion
// count, so the guest sees these fields). The owner is the KTHREAD (PCR + 0x100). Unlike Xenia, a
// null critical section stops instead of being ignored or reported as acquired.
#include "critical_section.h"
#include "dispatcher.h"
#include "imports.h"
#include "thread_state.h"
#include <atomic>

namespace {

using namespace runtime;

constexpr uint32_t kLockCount = 0x10, kRecursionCount = 0x14, kOwner = 0x18;

std::atomic_ref<uint32_t> Word(uint8_t* base, uint32_t address)
{
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + address));
}
int32_t Get(uint8_t* base, uint32_t address) { return int32_t(__builtin_bswap32(Word(base, address).load())); }
void Set(uint8_t* base, uint32_t address, int32_t value) { Word(base, address).store(__builtin_bswap32(uint32_t(value))); }
// Adds to a big-endian word atomically; returns the new value.
int32_t Add(uint8_t* base, uint32_t address, int32_t delta)
{
    auto word = Word(base, address);
    uint32_t old = word.load();
    while (!word.compare_exchange_weak(old, __builtin_bswap32(uint32_t(int32_t(__builtin_bswap32(old)) + delta))))
        ;
    return int32_t(__builtin_bswap32(old)) + delta;
}
bool CompareExchange(uint8_t* base, uint32_t address, int32_t expected, int32_t desired)
{
    uint32_t e = __builtin_bswap32(uint32_t(expected));
    return Word(base, address).compare_exchange_strong(e, __builtin_bswap32(uint32_t(desired)));
}

uint32_t Section(const PPCContext& ctx, const char* function)
{
    const uint32_t cs = ctx.r3.u32;
    if (cs == 0 || (cs & 3))
        Stop(StopCode::GuestFault, "%s on the critical section at %08X (null or misaligned)", function, cs);
    return cs;
}

}  // namespace

void runtime::InitializeCriticalSection(uint8_t* base, uint32_t cs, uint32_t spinCount)
{
    // Rounded up to 256s and capped at 255. Computed in 64 bits: Xenia's 32-bit sum wraps for counts
    // above 0xFFFFFF00 (giving 0); the capped value is INFERRED for those.
    const uint64_t spin = (uint64_t(spinCount) + 255) >> 8;
    base[cs] = dispatch::kSynchronizationEvent;
    base[cs + 1] = uint8_t(spin > 255 ? 255 : spin);  // spin count / 256, rounded up
    Set(base, cs + dispatch::kSignalState, 0);
    Set(base, cs + kLockCount, -1);
    Set(base, cs + kRecursionCount, 0);
    Set(base, cs + kOwner, 0);
}


GUEST_IMPORT(RtlInitializeCriticalSection, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc; all fields big-endian (user decision)")
{
    InitializeCriticalSection(base, Section(ctx, "RtlInitializeCriticalSection"), 0);
}

GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc; all fields big-endian (user decision)")
{
    InitializeCriticalSection(base, Section(ctx, "RtlInitializeCriticalSectionAndSpinCount"), ctx.r4.u32);
    ctx.r3.u64 = 0;  // STATUS_SUCCESS
}

GUEST_IMPORT(RtlEnterCriticalSection, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc; all fields big-endian (user decision)")
{
    const uint32_t cs = Section(ctx, "RtlEnterCriticalSection");
    const uint32_t self = CurrentThread(ctx, base);
    if (uint32_t(Get(base, cs + kOwner)) == self)
    {
        Add(base, cs + kLockCount, 1);
        Set(base, cs + kRecursionCount, Get(base, cs + kRecursionCount) + 1);
        return;
    }
    for (uint32_t spin = base[cs + 1] * 256u; spin; spin--)
    {
        if (CompareExchange(base, cs + kLockCount, -1, 0))
        {
            Set(base, cs + kOwner, int32_t(self));
            Set(base, cs + kRecursionCount, 1);
            return;
        }
    }
    if (Add(base, cs + kLockCount, 1) != 0)
        WaitForEvent(base, cs);  // the owner's Leave signals the event for one waiter
    Set(base, cs + kOwner, int32_t(self));
    Set(base, cs + kRecursionCount, 1);
}

GUEST_IMPORT(RtlTryEnterCriticalSection, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc; all fields big-endian (user decision)")
{
    const uint32_t cs = Section(ctx, "RtlTryEnterCriticalSection");
    const uint32_t self = CurrentThread(ctx, base);
    if (CompareExchange(base, cs + kLockCount, -1, 0))
    {
        Set(base, cs + kOwner, int32_t(self));
        Set(base, cs + kRecursionCount, 1);
        ctx.r3.u64 = 1;
    }
    else if (uint32_t(Get(base, cs + kOwner)) == self)
    {
        Add(base, cs + kLockCount, 1);
        Set(base, cs + kRecursionCount, Get(base, cs + kRecursionCount) + 1);
        ctx.r3.u64 = 1;
    }
    else
        ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlLeaveCriticalSection, "REFERENCE_DERIVED: Xenia xboxkrnl_rtl.cc; all fields big-endian (user decision)")
{
    const uint32_t cs = Section(ctx, "RtlLeaveCriticalSection");
    if (uint32_t(Get(base, cs + kOwner)) != CurrentThread(ctx, base) || Get(base, cs + kRecursionCount) <= 0)
        Stop(StopCode::GuestFault, "RtlLeaveCriticalSection on %08X by a thread that does not own it", cs);
    const int32_t recursion = Get(base, cs + kRecursionCount) - 1;
    Set(base, cs + kRecursionCount, recursion);
    if (recursion != 0)
    {
        Add(base, cs + kLockCount, -1);
        return;
    }
    Set(base, cs + kOwner, 0);
    if (Add(base, cs + kLockCount, -1) != -1)
        SetEvent(base, cs);  // waiters remain: wake one
}
