// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_sync.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Synchronization object imports (M3-T04/T05). REFERENCE_DERIVED from Xenia (xenia-canary
// 67f16c4e315f: kernel/xboxkrnl/xboxkrnl_threading.cc), re-derived over objects.h and dispatcher.h.
#include "arena.h"
#include "dispatcher.h"
#include "imports.h"
#include "objects.h"

namespace {

// A named object that already exists: a new handle to it (OBJECT_NAME_EXISTS) if its type
// matches, else INVALID_HANDLE (as Xenia). Returns false if the name is new (or empty).
bool OpenExisting(PPCContext& ctx, uint8_t* base, const std::string& name, runtime::ObjectType type, uint32_t handleOut)
{
    using namespace runtime;
    ObjectType existing{};
    const auto handle = LookupName(name, existing);
    if (!handle)
        return false;
    if (existing != type || !handleOut)
        CloseHandle(*handle);
    if (existing != type)
    {
        ctx.r3.u64 = status::kInvalidHandle;
        return true;
    }
    if (handleOut)
        Store32(base, handleOut, *handle);
    ctx.r3.u64 = status::kObjectNameExists;
    return true;
}

}  // namespace

GUEST_IMPORT(NtCreateEvent, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtCreateEvent")
{
    using namespace runtime;
    const uint32_t handleOut = ctx.r3.u32, attributes = ctx.r4.u32, type = ctx.r5.u32, initial = ctx.r6.u32;
    if (type > dispatch::kSynchronizationEvent)
        Stop(StopCode::Unimplemented, "NtCreateEvent: event type %u is neither notification (0) nor synchronization (1)", type);
    const std::string name = ObjectName(base, attributes);
    if (OpenExisting(ctx, base, name, ObjectType::Event, handleOut))
        return;
    const uint32_t header = ArenaAlloc(base, 16);  // X_KEVENT: a dispatcher header
    base[header] = uint8_t(type);
    Store32(base, header + dispatch::kSignalState, initial ? 1 : 0);
    const uint32_t handle = CreateObject(ObjectType::Event, header, name);
    if (handleOut)
        Store32(base, handleOut, handle);
    ctx.r3.u64 = status::kSuccess;
}

GUEST_IMPORT(NtCreateSemaphore, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtCreateSemaphore")
{
    // (handle*, attributes, count, limit): X_KSEMAPHORE = a dispatcher header (type 5, signal
    // state = count) and the limit at +0x10.
    using namespace runtime;
    const uint32_t handleOut = ctx.r3.u32, attributes = ctx.r4.u32;
    const int32_t count = int32_t(ctx.r5.u32), limit = int32_t(ctx.r6.u32);
    const std::string name = ObjectName(base, attributes);
    if (OpenExisting(ctx, base, name, ObjectType::Semaphore, handleOut))
        return;
    if (count < 0 || limit <= 0 || count > limit)
    {
        if (handleOut)
            Store32(base, handleOut, 0);
        ctx.r3.u64 = 0xC000000D;  // STATUS_INVALID_PARAMETER
        return;
    }
    const uint32_t header = ArenaAlloc(base, 20);
    base[header] = dispatch::kSemaphore;
    Store32(base, header + dispatch::kSignalState, uint32_t(count));
    Store32(base, header + dispatch::kSemaphoreLimit, uint32_t(limit));
    const uint32_t handle = CreateObject(ObjectType::Semaphore, header, name);
    if (handleOut)
        Store32(base, handleOut, handle);
    ctx.r3.u64 = status::kSuccess;
}

GUEST_IMPORT(NtReleaseSemaphore, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtReleaseSemaphore; limit check from NT (INFERRED)")
{
    // (handle, release count, previous count*). Passing the limit returns
    // STATUS_SEMAPHORE_LIMIT_EXCEEDED with the count unchanged (NT semantics; Xenia does not check).
    using namespace runtime;
    const auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != ObjectType::Semaphore)
    {
        ctx.r3.u64 = object ? status::kObjectTypeMismatch : status::kInvalidHandle;
        return;
    }
    int32_t previous = 0;
    const bool released = ReleaseSemaphore(base, object->header, int32_t(ctx.r4.u32), previous);
    if (ctx.r5.u32)
        Store32(base, ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = released ? status::kSuccess : 0xC0000047;
}

GUEST_IMPORT(NtCreateMutant, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtCreateMutant; KMUTANT layout and ownership from NT (INFERRED)")
{
    // (handle*, attributes, initial owner): a KMUTANT (dispatcher type 2; owner at +0x18): free
    // (signal state 1), or owned once by the calling thread (signal state 0). A name that exists:
    // as for events.
    using namespace runtime;
    const uint32_t handleOut = ctx.r3.u32, attributes = ctx.r4.u32, initialOwner = ctx.r5.u32;
    const std::string name = ObjectName(base, attributes);
    if (OpenExisting(ctx, base, name, ObjectType::Mutant, handleOut))
        return;
    const uint32_t header = ArenaAlloc(base, 0x20);
    base[header] = dispatch::kMutant;
    Store32(base, header + dispatch::kSignalState, initialOwner ? 0 : 1);
    Store32(base, header + kMutantOwner, initialOwner ? CurrentThread(ctx, base) : 0);
    const uint32_t handle = CreateObject(ObjectType::Mutant, header, name);
    if (handleOut)
        Store32(base, handleOut, handle);
    ctx.r3.u64 = status::kSuccess;
}

GUEST_IMPORT(NtReleaseMutant, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc NtReleaseMutant; ownership and previous count from NT (INFERRED)")
{
    // (handle, previous count*): releases one level held by the calling thread; the previous
    // signal state is written if asked for. Not the owner: STATUS_MUTANT_NOT_OWNED, nothing
    // changed (NT; Xenia's host mutex fails the same way).
    using namespace runtime;
    const auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != ObjectType::Mutant)
    {
        ctx.r3.u64 = object ? status::kObjectTypeMismatch : status::kInvalidHandle;
        return;
    }
    int32_t previous = 0;
    const bool released = ReleaseMutant(base, object->header, CurrentThread(ctx, base), previous);
    if (const uint32_t previousOut = ctx.r4.u32)
        Store32(base, previousOut, uint32_t(previous));
    ctx.r3.u64 = released ? status::kSuccess : 0xC0000046;  // STATUS_MUTANT_NOT_OWNED
}

GUEST_IMPORT(NtSetEvent, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc xeNtSetEvent")
{
    // (handle, previous state*): sets an event and wakes its waiters; the previous state is
    // written if asked for. An unknown handle is STATUS_INVALID_HANDLE, a handle to another object
    // type STATUS_OBJECT_TYPE_MISMATCH (Xenia; NT's ObReferenceObjectByHandle does the same).
    using namespace runtime;
    const auto object = LookupHandle(ctx.r3.u32);
    if (!object || object->type != ObjectType::Event)
    {
        ctx.r3.u64 = object ? status::kObjectTypeMismatch : status::kInvalidHandle;  // not an event
        return;
    }
    const int32_t previous = SetEvent(base, object->header);
    if (ctx.r4.u32)
        Store32(base, ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NtClearEvent, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc xeNtClearEvent; type mismatch from NT (INFERRED)")
{
    // (handle): resets an event. An unknown handle is STATUS_INVALID_HANDLE (Xenia); a handle to
    // another object type is STATUS_OBJECT_TYPE_MISMATCH, as NT's object lookup reports it (Xenia
    // does not check the type here).
    using namespace runtime;
    const auto object = LookupHandle(ctx.r3.u32);
    if (!object.has_value() || object->type != ObjectType::Event)
    {
        ctx.r3.u64 = object ? status::kObjectTypeMismatch : status::kInvalidHandle;  // only events reset
        return;
    }
    ResetEvent(base, object->header);
    ctx.r3.u64 = 0;
}

namespace {

// The KEVENT at a guest pointer (type 0 notification or 1 synchronization); anything else stops.
uint32_t GuestEvent(const uint8_t* base, uint32_t event, const char* function)
{
    if (event == 0 || event % 4 || base[event] > runtime::dispatch::kSynchronizationEvent)
        Stop(StopCode::Unimplemented, "%s(%08X): not an event object", function, event);
    return event;
}

}  // namespace

GUEST_IMPORT(KeSetEvent, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeSetEvent")
{
    // (event*, priority increment, wait): sets the event and wakes its waiters; returns the
    // previous state. The increment has no effect here (no priority boosts); `wait` (the caller
    // waits next with the dispatcher still locked) needs nothing, as each wait locks for itself.
    ctx.r3.u64 = uint32_t(runtime::SetEvent(base, GuestEvent(base, ctx.r3.u32, "KeSetEvent")));
}

GUEST_IMPORT(KeResetEvent, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc KeResetEvent")
{
    // (event*): clears the event; returns the previous state.
    ctx.r3.u64 = uint32_t(runtime::ResetEvent(base, GuestEvent(base, ctx.r3.u32, "KeResetEvent")));
}
