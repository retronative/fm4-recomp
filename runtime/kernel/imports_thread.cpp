// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_thread.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Thread and process imports (M3-T04). REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// kernel/xboxkrnl/xboxkrnl_threading.cc), re-derived over the guest state of thread_state.h.
#include "imports.h"
#include "objects.h"
#include "thread_state.h"
#include "threads.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace runtime {

// The calling thread's process block: PCR (r13) + 0x100 -> KTHREAD, KTHREAD + 0x84 -> process.
inline uint32_t CurrentProcess(const PPCContext& ctx, const uint8_t* base)
{
    return Load32(base, Load32(base, ctx.r13.u32 + pcr::kCurrentThread) + kthread::kProcess);
}

// A thread's dynamic TLS slots follow its static TLS data (KTHREAD + 0x68, process block TLS
// data size); the process block holds the slot area size (the executable's slot count x 4).
inline uint32_t SlotCount(const uint8_t* base, uint32_t process) { return (base[process + process::kTlsSlotSize] << 8 | base[process + process::kTlsSlotSize + 1]) / 4; }
inline uint32_t SlotAddress(const uint8_t* base, uint32_t kthread, uint32_t slot)
{
    const uint32_t process = Load32(base, kthread + kthread::kProcess);
    return Load32(base, kthread + kthread::kTls) + Load32(base, process + process::kTlsDataSize) + slot * 4;
}

}  // namespace runtime

namespace {

constexpr uint32_t kTlsOutOfIndexes = 0xFFFFFFFF;
std::mutex slotsLock;
std::vector<bool> slotsInUse;

uint32_t CheckedSlot(const PPCContext& ctx, const uint8_t* base, uint32_t slot, const char* function)
{
    const uint32_t count = runtime::SlotCount(base, runtime::CurrentProcess(ctx, base));
    if (slot >= count)
        Stop(StopCode::GuestFault, "%s(%u): the executable has %u TLS slots", function, slot, count);
    return runtime::SlotAddress(base, runtime::Load32(base, ctx.r13.u32 + runtime::pcr::kCurrentThread), slot);
}

}  // namespace

GUEST_IMPORT(KeGetCurrentProcessType, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeGetCurrentProcessType (process block type)")
{
    ctx.r3.u64 = base[runtime::CurrentProcess(ctx, base) + runtime::process::kProcessType];
}

// Dynamic TLS (REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeTls*, xthread.cc Get/SetTLSValue):
// slots are bounded by the executable's slot count; Xenia's larger bitmap and unchecked overflow
// are not reproduced (a slot past the count stops, allocation past it returns TLS_OUT_OF_INDEXES).
GUEST_IMPORT(KeTlsAlloc, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeTlsAlloc; slots bounded by the XEX slot count")
{
    std::lock_guard guard(slotsLock);
    slotsInUse.resize(runtime::SlotCount(base, runtime::CurrentProcess(ctx, base)));
    for (uint32_t slot = 0; slot < slotsInUse.size(); slot++)
    {
        if (slotsInUse[slot])
            continue;
        slotsInUse[slot] = true;
        runtime::Store32(base, CheckedSlot(ctx, base, slot, "KeTlsAlloc"), 0);
        ctx.r3.u64 = slot;
        return;
    }
    ctx.r3.u64 = kTlsOutOfIndexes;
}

GUEST_IMPORT(KeTlsFree, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeTlsFree (clears the slot in every thread)")
{
    const uint32_t slot = ctx.r3.u32;
    if (slot == kTlsOutOfIndexes)
    {
        ctx.r3.u64 = 0;
        return;
    }
    CheckedSlot(ctx, base, slot, "KeTlsFree");
    std::lock_guard guard(slotsLock);
    for (uint32_t thread : runtime::AllThreads())
        runtime::Store32(base, runtime::SlotAddress(base, thread, slot), 0);
    if (slot < slotsInUse.size())
        slotsInUse[slot] = false;
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(KeTlsGetValue, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeTlsGetValue")
{
    ctx.r3.u64 = runtime::Load32(base, CheckedSlot(ctx, base, ctx.r3.u32, "KeTlsGetValue"));
}

GUEST_IMPORT(KeTlsSetValue, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeTlsSetValue")
{
    runtime::Store32(base, CheckedSlot(ctx, base, ctx.r3.u32, "KeTlsSetValue"), ctx.r4.u32);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(ExCreateThread, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc ExCreateThread, xthread.cc Execute")
{
    // ExCreateThread(handle*, stack size, thread id*, XAPI startup, start, context, flags).
    const uint32_t handleOut = ctx.r3.u32, idOut = ctx.r5.u32, flags = ctx.r9.u32;
    uint32_t threadId = 0;
    const uint32_t kthread = runtime::CreateGuestThread(base, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, flags, threadId);
    if (handleOut)  // flag 0x80 asks for the KTHREAD itself instead of a handle (Xenia)
        runtime::Store32(base, handleOut, (flags & 0x80) ? kthread : runtime::CreateObject(runtime::ObjectType::Thread, kthread, ""));
    if (idOut)
        runtime::Store32(base, idOut, threadId);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(KeSetAffinityThread, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeSetAffinityThread, xthread.cc SetActiveCpu")
{
    // The previous affinity goes through the third argument, the status is the result (Xenia, from
    // the console's disassembly). The thread moves to the highest CPU in the mask (PCR and KTHREAD);
    // host threads are not pinned (macOS has no affinity), so host scheduling is unchanged.
    const uint32_t kthread = ctx.r3.u32, affinity = ctx.r4.u32, previousOut = ctx.r5.u32;
    if (affinity == 0)
    {
        ctx.r3.u64 = 0xC000000D;  // STATUS_INVALID_PARAMETER
        return;
    }
    runtime::ThreadState thread{};
    if (!runtime::FindThread(kthread, thread))
        Stop(StopCode::GuestFault, "KeSetAffinityThread on %08X, which is no thread", kthread);
    const uint32_t cpu = 31 - __builtin_clz(affinity);
    if (cpu >= 6)
        Stop(StopCode::Unimplemented, "KeSetAffinityThread: affinity %08X names a CPU past the six hardware threads", affinity);
    if (previousOut)
        runtime::Store32(base, previousOut, 1u << base[kthread + runtime::kthread::kCurrentCpu]);
    base[kthread + runtime::kthread::kCurrentCpu] = uint8_t(cpu);
    base[thread.pcr + runtime::pcr::kCurrentCpu] = uint8_t(cpu);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(KiApcNormalRoutineNop, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KiApcNormalRoutineNop (returns 0); a no-op by its name")
{
    // The kernel's APC normal routine that does nothing (titles call it directly or pass it as an
    // APC routine); r3 is cleared as Xenia's returns 0.
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(KeInitializeDpc, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeInitializeDpc (XDPC layout)")
{
    // (dpc*, routine, context): a deferred procedure call object, 28 bytes big-endian: type 19
    // (DpcObject) << 24 with importance and number 0, list links 0, routine, context, two
    // arguments 0. Queuing one (KeInsertQueueDpc) is not implemented and stops: nothing would run it.
    const uint32_t dpc = ctx.r3.u32;
    const uint32_t words[7] = {19u << 24, 0, 0, ctx.r4.u32, ctx.r5.u32, 0, 0};
    for (uint32_t i = 0; i < 7; i++)
        runtime::Store32(base, dpc + 4 * i, words[i]);
}

GUEST_IMPORT(KeSetBasePriorityThread, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeSetBasePriorityThread, xthread.cc SetPriority (KTHREAD +0x70); previous value from NT (INFERRED)")
{
    // (KTHREAD, increment): the increment goes to the KTHREAD's priority byte (+0x70, as Xenia)
    // and the previous one is returned (NT's documented result; Xenia returns its host thread's
    // priority class instead). Host scheduling priority is not changed. An unknown thread stops.
    runtime::ThreadState state{};
    if (!runtime::FindThread(ctx.r3.u32, state))
        Stop(StopCode::Unimplemented, "KeSetBasePriorityThread(%08X): not a thread the runtime created", ctx.r3.u32);
    const int8_t previous = int8_t(base[state.kthread + runtime::kthread::kPriority]);
    base[state.kthread + runtime::kthread::kPriority] = uint8_t(ctx.r4.u32);
    ctx.r3.u64 = uint64_t(int64_t(previous));
}

GUEST_IMPORT(KeQueryBasePriorityThread, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeQueryBasePriorityThread (KTHREAD +0x70, as KeSetBasePriorityThread stores it)")
{
    // (KTHREAD): its priority byte (+0x70), sign-extended. An unknown thread stops (Xenia returns 0).
    runtime::ThreadState state{};
    if (!runtime::FindThread(ctx.r3.u32, state))
        Stop(StopCode::Unimplemented, "KeQueryBasePriorityThread(%08X): not a thread the runtime created", ctx.r3.u32);
    ctx.r3.u64 = uint64_t(int64_t(int8_t(base[state.kthread + runtime::kthread::kPriority])));
}

namespace {

// The calling thread's KTHREAD APC-disable count (+0xB0), changed by `delta`. Big-endian, as every
// guest-visible field here (Xenia keeps it in host order). No APC is ever queued, so reaching 0
// delivers nothing.
void ChangeApcDisable(const PPCContext& ctx, uint8_t* base, int32_t delta)
{
    const uint32_t kthread = runtime::Load32(base, ctx.r13.u32 + runtime::pcr::kCurrentThread);
    const uint32_t field = kthread + runtime::kthread::kApcDisableCount;
    runtime::Store32(base, field, uint32_t(int32_t(runtime::Load32(base, field)) + delta));
}

}  // namespace

GUEST_IMPORT(KeEnterCriticalRegion, "REFERENCE_DERIVED: Xenia xthread.cc EnterCriticalRegion (KTHREAD +0xB0 - 1; big-endian)")
{
    ChangeApcDisable(ctx, base, -1);
}

GUEST_IMPORT(KeLeaveCriticalRegion, "REFERENCE_DERIVED: Xenia xthread.cc LeaveCriticalRegion (KTHREAD +0xB0 + 1; big-endian)")
{
    ChangeApcDisable(ctx, base, +1);
}

GUEST_IMPORT(NtYieldExecution, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtYieldExecution (a zero delay, returns 0)")
{
    // Gives up the rest of the time slice (a host yield) and reports success.
    std::this_thread::yield();
    ctx.r3.u64 = 0;
}

namespace {

// Spin locks hold the owner's PCR address (r13), big-endian; 0 is free (Xenia). The IRQL is the
// PCR's current_irql byte (+0x18), as KeRaiseIrqlToDpcLevel uses it (Xenia keeps a host copy for
// spin locks; one guest-visible field is used here for both).
std::atomic_ref<uint32_t> SpinLock(uint8_t* base, uint32_t lock)
{
    if (lock == 0 || lock % 4)
        Stop(StopCode::GuestFault, "spin lock at %08X (null or misaligned)", lock);
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + lock));
}

void AcquireSpinLock(const PPCContext& ctx, uint8_t* base, uint32_t lock, const char* function)
{
    auto word = SpinLock(base, lock);
    const uint32_t owner = __builtin_bswap32(ctx.r13.u32);
    if (word.load() == owner)
        Stop(StopCode::Unimplemented, "%s(%08X): the lock is already held by this processor (it would spin forever)", function, lock);
    for (uint32_t expected = 0; !word.compare_exchange_weak(expected, owner, std::memory_order_acquire); expected = 0)
        std::this_thread::yield();
}

void ReleaseSpinLock(const PPCContext& ctx, uint8_t* base, uint32_t lock, const char* function)
{
    auto word = SpinLock(base, lock);
    if (word.load() != __builtin_bswap32(ctx.r13.u32))
        Stop(StopCode::Unimplemented, "%s(%08X): released by a processor that does not hold it", function, lock);
    word.store(0, std::memory_order_release);
}

}  // namespace

GUEST_IMPORT(KfAcquireSpinLock, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc xeKeKfAcquireSpinLock (owner r13, IRQL 2)")
{
    // (lock*): spins until free, takes it, raises the IRQL to DISPATCH (2); returns the old IRQL.
    AcquireSpinLock(ctx, base, ctx.r3.u32, "KfAcquireSpinLock");
    const uint32_t irql = ctx.r13.u32 + runtime::pcr::kCurrentIrql;
    const uint8_t previous = base[irql];
    base[irql] = 2;
    ctx.r3.u64 = previous;
}

GUEST_IMPORT(KfReleaseSpinLock, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KfReleaseSpinLock")
{
    // (lock*, old IRQL): frees the lock and lowers the IRQL back when it was below DISPATCH.
    ReleaseSpinLock(ctx, base, ctx.r3.u32, "KfReleaseSpinLock");
    if (ctx.r4.u32 < 2)
        base[ctx.r13.u32 + runtime::pcr::kCurrentIrql] = uint8_t(ctx.r4.u32);
}

GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeAcquireSpinLockAtRaisedIrql")
{
    AcquireSpinLock(ctx, base, ctx.r3.u32, "KeAcquireSpinLockAtRaisedIrql");
}

GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeReleaseSpinLockFromRaisedIrql")
{
    ReleaseSpinLock(ctx, base, ctx.r3.u32, "KeReleaseSpinLockFromRaisedIrql");
}

GUEST_IMPORT(KeDelayExecutionThread, "REFERENCE_DERIVED: Xenia xthread.cc Delay; absolute times from NT (INFERRED)")
{
    // (processor mode, alertable, interval*): sleeps for the interval in 100 ns units (negative:
    // relative; positive: until that system time, which Xenia does not handle; 0 or none: a
    // yield) and returns STATUS_SUCCESS. No APC can be queued yet, so an alertable delay is never
    // interrupted.
    const uint32_t intervalAddress = ctx.r5.u32;
    int64_t interval = 0;
    if (intervalAddress)
        interval = int64_t(uint64_t(runtime::Load32(base, intervalAddress)) << 32 | runtime::Load32(base, intervalAddress + 4));
    uint64_t ticks = 0;
    if (interval < 0)
        ticks = 0 - uint64_t(interval);
    else if (interval > 0)
    {
        const uint64_t now = runtime::GuestSystemTime();
        ticks = uint64_t(interval) > now ? uint64_t(interval) - now : 0;
    }
    if (ticks == 0)
        std::this_thread::yield();
    else
        std::this_thread::sleep_for(std::chrono::nanoseconds(int64_t(std::min<uint64_t>(ticks, uint64_t(INT64_MAX) / 100)) * 100));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(ExTerminateThread, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc ExTerminateThread")
{
    // (exit code): ends the calling guest thread; its object is signalled with the exit status.
    // Never returns (threads.h ExitCurrentGuestThread; a runtime service thread stops instead).
    runtime::ExitCurrentGuestThread(ctx.r3.u32);
}

GUEST_IMPORT(KeResumeThread, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc:213-224 KeResumeThread (by KTHREAD); result: previous suspend count from NT (INFERRED)")
{
    // (KTHREAD*): decrements the thread's suspend count and starts it at zero (threads.h ResumeGuestThread), as
    // NtResumeThread does by handle. Returns the previous suspend count, NT's documented result (Xenia returns
    // X_STATUS_SUCCESS instead; FM4's two call sites do not use the result, OBSERVED in the translation). A KTHREAD the
    // runtime did not create stops (Xenia: INVALID_HANDLE), as KeSetBasePriorityThread does.
    runtime::ThreadState state{};
    if (!runtime::FindThread(ctx.r3.u32, state))
        Stop(StopCode::Unimplemented, "KeResumeThread(%08X): not a thread the runtime created (lr=%08X)", ctx.r3.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = runtime::ResumeGuestThread(base, state.kthread);
}
