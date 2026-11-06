// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_object.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Object manager imports (M3-T04). REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// kernel/xboxkrnl/xboxkrnl_ob.cc), re-derived over objects.h. An object type is identified by the
// address in its Ex*ObjectType import slot; the type objects' contents are undocumented (their
// slots point at guard pages, so reading one stops).
#include "imports.h"
#include "module.h"
#include "objects.h"
#include "thread_state.h"
#include "threads.h"
#include "dispatcher.h"

namespace runtime {

constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;  // NtCurrentThread()

// The calling guest thread's KTHREAD (its PCR's current thread).
uint32_t CurrentThread(const PPCContext& ctx, const uint8_t* base) { return Load32(base, ctx.r13.u32 + pcr::kCurrentThread); }

namespace {
// The waiting thread for the dispatcher: 0 (unknown: a mutant wait then stops) without a PCR.
uint32_t Waiter(const PPCContext& ctx, const uint8_t* base) { return ctx.r13.u32 ? CurrentThread(ctx, base) : 0; }
}  // namespace

// The object a handle names, the current-thread pseudo-handle included.
std::optional<KernelObject> ResolveHandle(const PPCContext& ctx, const uint8_t* base, uint32_t handle)
{
    if (handle == kCurrentThreadHandle)
        return KernelObject{ObjectType::Thread, Load32(base, ctx.r13.u32 + pcr::kCurrentThread)};
    return LookupHandle(handle);
}

uint32_t ObjectTypeAddress(ObjectType type)
{
    switch (type)  // Xenia xboxkrnl_ob.cc object_types
    {
    case ObjectType::Thread: return DataImportValue("ExThreadObjectType");
    case ObjectType::Semaphore: return DataImportValue("ExSemaphoreObjectType");
    case ObjectType::File: return DataImportValue("IoFileObjectType");
    case ObjectType::Event: break;
    case ObjectType::Enumerator:
    case ObjectType::NotifyListener: return 0;  // XAM objects: no kernel object type matches them
    case ObjectType::Timer:                     // FM4 imports no ExTimerObjectType
    case ObjectType::Mutant: return 0;          // nor ExMutantObjectType
    }
    return DataImportValue("ExEventObjectType");
}

}  // namespace runtime

GUEST_IMPORT(ObReferenceObjectByHandle, "REFERENCE_DERIVED: Xenia xboxkrnl_ob.cc ObReferenceObjectByHandle")
{
    using namespace runtime;
    const uint32_t handle = ctx.r3.u32, typeAddress = ctx.r4.u32, out = ctx.r5.u32;
    Store32(base, out, 0);  // the kernel clears it first (Xenia)
    const auto object = ResolveHandle(ctx, base, handle);
    if (!object)
    {
        ctx.r3.u64 = status::kInvalidHandle;
        return;
    }
    const uint32_t expected = ObjectTypeAddress(object->type);
    if (typeAddress && expected == 0)  // the executable does not import that type: nothing to compare with
        Stop(StopCode::Unimplemented, "ObReferenceObjectByHandle: no type export for this object's type");
    if (typeAddress && typeAddress != expected)
    {
        ctx.r3.u64 = status::kObjectTypeMismatch;
        return;
    }
    Store32(base, out, object->header);  // objects live for the whole run: references are not counted
    ctx.r3.u64 = status::kSuccess;
}

GUEST_IMPORT(ObDereferenceObject, "REFERENCE_DERIVED: Xenia xboxkrnl_ob.cc ObDereferenceObject")
{
    // Objects live for the whole run, so a dereference only checks its argument: a null or unknown
    // object pointer stops (Xenia logs and ignores it).
    const uint32_t object = ctx.r3.u32;
    runtime::ThreadState thread{};
    if (object == 0 || !(runtime::IsObjectBody(object) || runtime::FindThread(object, thread)))
        Stop(StopCode::GuestFault, "ObDereferenceObject(%08X): not an object the kernel handed out", object);
}

GUEST_IMPORT(NtResumeThread, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtResumeThread")
{
    const uint32_t handle = ctx.r3.u32, countOut = ctx.r4.u32;
    const auto object = runtime::ResolveHandle(ctx, base, handle);
    if (!object || object->type != runtime::ObjectType::Thread)
    {
        ctx.r3.u64 = object ? runtime::status::kObjectTypeMismatch : runtime::status::kInvalidHandle;
        return;
    }
    const uint32_t previous = runtime::ResumeGuestThread(base, object->header);
    if (countOut)
        runtime::Store32(base, countOut, previous);
    ctx.r3.u64 = runtime::status::kSuccess;
}

GUEST_IMPORT(NtSuspendThread, "INFERRED: NT NtSuspendThread semantics (suspend count; the caller itself or a thread not yet started)")
{
    // (handle, previous count*): see SuspendGuestThread; the previous count is written if asked.
    const uint32_t handle = ctx.r3.u32, countOut = ctx.r4.u32;
    const auto object = runtime::ResolveHandle(ctx, base, handle);
    if (!object || object->type != runtime::ObjectType::Thread)
    {
        ctx.r3.u64 = object ? runtime::status::kObjectTypeMismatch : runtime::status::kInvalidHandle;
        return;
    }
    uint32_t previous = 0;
    const uint32_t result = runtime::SuspendGuestThread(base, object->header, runtime::CurrentThread(ctx, base), previous);
    if (countOut)
        runtime::Store32(base, countOut, previous);
    ctx.r3.u64 = result;
}

GUEST_IMPORT(NtDuplicateObject, "REFERENCE_DERIVED: Xenia xboxkrnl_ob.cc NtDuplicateObject")
{
    // A new handle to the same object (the current-thread pseudo-handle gives a real handle to the
    // calling thread); DUPLICATE_CLOSE_SOURCE (1) closes the source handle.
    const uint32_t handle = ctx.r3.u32, out = ctx.r4.u32, options = ctx.r5.u32;
    std::optional<uint32_t> duplicate;
    if (handle == runtime::kCurrentThreadHandle)
        duplicate = runtime::HandleForObject(runtime::ObjectType::Thread,
                                             runtime::Load32(base, ctx.r13.u32 + runtime::pcr::kCurrentThread));
    else
        duplicate = runtime::DuplicateHandle(handle);
    if (!duplicate)
    {
        ctx.r3.u64 = runtime::status::kInvalidHandle;
        return;
    }
    if (out)
        runtime::Store32(base, out, *duplicate);
    else
        runtime::CloseHandle(*duplicate);
    if (options == 1 && handle != runtime::kCurrentThreadHandle)
        runtime::CloseHandle(handle);
    ctx.r3.u64 = runtime::status::kSuccess;
}

GUEST_IMPORT(NtWaitForSingleObjectEx, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtWaitForSingleObjectEx, xobject.cc Wait")
{
    // (handle, wait mode, alertable, timeout*). No APC can be queued yet, so an alertable wait
    // cannot be interrupted (queuing one stops).
    const uint32_t handle = ctx.r3.u32, timeoutAddress = ctx.r6.u32;
    const auto object = runtime::ResolveHandle(ctx, base, handle);
    if (!object)
    {
        ctx.r3.u64 = runtime::status::kInvalidHandle;
        return;
    }
    int64_t timeout = 0;
    if (timeoutAddress)
        timeout = int64_t(uint64_t(runtime::Load32(base, timeoutAddress)) << 32 | runtime::Load32(base, timeoutAddress + 4));
    ctx.r3.u64 = runtime::WaitForObject(base, object->header, timeoutAddress ? &timeout : nullptr, runtime::Waiter(ctx, base)) ? runtime::wait::kSuccess
                                                                                                 : runtime::wait::kTimeout;
}

GUEST_IMPORT(NtClose, "REFERENCE_DERIVED: Xenia xboxkrnl_ob.cc NtClose")
{
    // Releases one handle; the object (in the kernel arena) lives on.
    ctx.r3.u64 = runtime::CloseHandle(ctx.r3.u32) ? runtime::status::kSuccess : runtime::status::kInvalidHandle;
}

GUEST_IMPORT(NtSignalAndWaitForSingleObjectEx, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtSignalAndWaitForSingleObjectEx, xobject.cc SignalAndWait")
{
    // (signal handle, wait handle, alertable, unused, timeout*; Xenia's names): signal one object and wait for
    // the other as one step (dispatcher.h). An unknown handle is STATUS_INVALID_HANDLE (Xenia).
    // No APC can be queued yet, so an alertable wait cannot be interrupted (queuing one stops).
    const uint32_t timeoutAddress = ctx.r7.u32;
    const auto signal = runtime::ResolveHandle(ctx, base, ctx.r3.u32);
    const auto object = runtime::ResolveHandle(ctx, base, ctx.r4.u32);
    if (!signal || !object)
    {
        ctx.r3.u64 = runtime::status::kInvalidHandle;
        return;
    }
    int64_t timeout = 0;
    if (timeoutAddress)
        timeout = int64_t(uint64_t(runtime::Load32(base, timeoutAddress)) << 32 | runtime::Load32(base, timeoutAddress + 4));
    ctx.r3.u64 = runtime::SignalAndWaitForObject(base, signal->header, object->header, timeoutAddress ? &timeout : nullptr, runtime::Waiter(ctx, base));
}

GUEST_IMPORT(NtWaitForMultipleObjectsEx, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtWaitForMultipleObjectsEx, xobject.cc WaitMultiple")
{
    // (count, handles*, wait type: 0 all, 1 any, wait mode, alertable, timeout*): a count of 0 or
    // above 64, another wait type or an unknown handle is STATUS_INVALID_PARAMETER (Xenia). No APC
    // can be queued yet, so an alertable wait cannot be interrupted (queuing one stops).
    const uint32_t count = ctx.r3.u32, handles = ctx.r4.u32, type = ctx.r5.u32, timeoutAddress = ctx.r8.u32;
    if (count == 0 || count > 64 || type > 1)
    {
        ctx.r3.u64 = 0xC000000D;  // STATUS_INVALID_PARAMETER
        return;
    }
    uint32_t headers[64];
    for (uint32_t i = 0; i < count; i++)
    {
        const auto object = runtime::ResolveHandle(ctx, base, runtime::Load32(base, handles + 4 * i));
        if (!object)
        {
            ctx.r3.u64 = 0xC000000D;
            return;
        }
        headers[i] = object->header;
    }
    int64_t timeout = 0;
    if (timeoutAddress)
        timeout = int64_t(uint64_t(runtime::Load32(base, timeoutAddress)) << 32 | runtime::Load32(base, timeoutAddress + 4));
    ctx.r3.u64 = runtime::WaitForObjects(base, headers, count, type == 0, timeoutAddress ? &timeout : nullptr, runtime::Waiter(ctx, base));
}

GUEST_IMPORT(KeWaitForMultipleObjects, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeWaitForMultipleObjects, xobject.cc WaitMultiple")
{
    // (count, objects*, wait type: 0 all, 1 any, wait reason, processor mode, alertable, timeout*,
    // wait blocks*): a wait on dispatcher objects in guest memory (dispatcher.h), as
    // NtWaitForMultipleObjectsEx with object pointers instead of handles. Xenia asserts the count
    // (1..64) and the wait type: other values are not modelled and stop. The kernel's wait blocks
    // are not used. No APC can be queued yet, so an alertable wait cannot be interrupted.
    const uint32_t count = ctx.r3.u32, objects = ctx.r4.u32, type = ctx.r5.u32, timeoutAddress = ctx.r9.u32;
    if (type > 1 || count == 0 || count > 64)
        Stop(StopCode::Unimplemented, "KeWaitForMultipleObjects(count %u, wait type %u) is not modelled", count, type);
    uint32_t headers[64];
    for (uint32_t i = 0; i < count; i++)
    {
        headers[i] = runtime::Load32(base, objects + 4 * i);
        if (headers[i] == 0)
            Stop(StopCode::GuestFault, "KeWaitForMultipleObjects on a null object (index %u)", i);
    }
    int64_t timeout = 0;
    if (timeoutAddress)
        timeout = int64_t(uint64_t(runtime::Load32(base, timeoutAddress)) << 32 | runtime::Load32(base, timeoutAddress + 4));
    ctx.r3.u64 = runtime::WaitForObjects(base, headers, count, type == 0, timeoutAddress ? &timeout : nullptr, runtime::Waiter(ctx, base));
}

GUEST_IMPORT(KeWaitForSingleObject,"REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeWaitForSingleObject, xobject.cc Wait")
{
    // (object*, wait reason, processor mode, alertable, timeout*): a wait on a dispatcher object
    // in guest memory (dispatcher.h; other object types stop there). No APC can be queued yet, so
    // an alertable wait cannot be interrupted.
    const uint32_t object = ctx.r3.u32, timeoutAddress = ctx.r7.u32;
    if (object == 0)
        Stop(StopCode::GuestFault, "KeWaitForSingleObject on a null object");
    int64_t timeout = 0;
    if (timeoutAddress)
        timeout = int64_t(uint64_t(runtime::Load32(base, timeoutAddress)) << 32 | runtime::Load32(base, timeoutAddress + 4));
    ctx.r3.u64 = runtime::WaitForObject(base, object, timeoutAddress ? &timeout : nullptr, runtime::Waiter(ctx, base)) ? runtime::wait::kSuccess
                                                                                         : runtime::wait::kTimeout;
}
