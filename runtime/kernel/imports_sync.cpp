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

// KeInitializeMutant and KeReleaseMutant. Xenia
// 67f16c4 has neither (xboxkrnl_table.inc lists them); the semantics are NT's (INFERRED) over the KMUTANT model
// NtCreateMutant already uses.
GUEST_IMPORT(KeInitializeMutant, "INFERRED: NT KeInitializeMutant (KMUTANT: type 2, wait list head, owner +0x18, abandoned +0x1C); Xenia has none")
{
    // (mutant*, initial owner): the 0x20-byte KMUTANT is cleared, then: type 2, signal state 1 (free) or 0 (owned
    // once by the calling thread, whose KTHREAD becomes the owner), the wait list head pointing at itself (+8, +0xC;
    // an empty NT list, as the title's own inline initialisation of dispatcher objects writes it). NT also links an
    // owned mutant into its owner's mutant list (for abandonment at thread exit); not modelled, as for NtCreateMutant.
    // A null or misaligned pointer stops.
    using namespace runtime;
    const uint32_t mutant = ctx.r3.u32, initialOwner = ctx.r4.u32;
    if (mutant == 0 || mutant % 4)
        Stop(StopCode::GuestFault, "KeInitializeMutant(%08X): null or misaligned (lr=%08X)", mutant, uint32_t(ctx.lr));
    std::memset(base + mutant, 0, 0x20);
    base[mutant] = dispatch::kMutant;
    Store32(base, mutant + dispatch::kSignalState, initialOwner ? 0 : 1);
    Store32(base, mutant + 8, mutant + 8);
    Store32(base, mutant + 0xC, mutant + 8);
    Store32(base, mutant + kMutantOwner, initialOwner ? CurrentThread(ctx, base) : 0);
}

GUEST_IMPORT(KeReleaseMutant, "INFERRED: NT KeReleaseMutant (previous signal state; not owned raises STATUS_MUTANT_NOT_OWNED); Xenia has none")
{
    // (mutant*, priority increment, abandoned, wait): releases one level held by the calling thread and returns the
    // signal state before (dispatcher.h ReleaseMutant). The increment has no effect (no priority boosts) and `wait`
    // needs nothing, as for KeSetEvent. A release by a thread that does not own the mutant makes NT raise an
    // exception (STATUS_MUTANT_NOT_OWNED), which is not modelled: it stops. Abandoning (NT: frees the mutant whoever
    // owns it and marks it abandoned) is not modelled and stops; so does an object that is not a mutant.
    using namespace runtime;
    const uint32_t mutant = ctx.r3.u32, abandoned = ctx.r5.u32;
    if (mutant == 0 || mutant % 4 || base[mutant] != dispatch::kMutant)
        Stop(StopCode::GuestFault, "KeReleaseMutant(%08X): not a mutant (lr=%08X)", mutant, uint32_t(ctx.lr));
    if (abandoned & 0xFF)  // BOOLEAN
        Stop(StopCode::Unimplemented, "KeReleaseMutant(%08X, abandoned): abandoning a mutant is not implemented (lr=%08X)", mutant,
             uint32_t(ctx.lr));
    int32_t previous = 0;
    if (!ReleaseMutant(base, mutant, CurrentThread(ctx, base), previous))
        Stop(StopCode::GuestFault, "KeReleaseMutant(%08X): not owned by the calling thread (signal state %d, owner %08X; NT raises "
             "STATUS_MUTANT_NOT_OWNED) (lr=%08X)", mutant, previous, Load32(base, mutant + kMutantOwner), uint32_t(ctx.lr));
    ctx.r3.u64 = uint32_t(previous);
}

namespace {
// X_SLIST_HEADER (Xenia xbox.h:291-296): next +0 (32 bits), depth +4 (16), sequence +6 (16), big-endian, 8-byte aligned.
// Updated with one 64-bit compare-and-swap on the raw bytes, as the translation's stdcx. does (XenonRecomp
// PPC_INST_STDCX), so these imports and the title's own reservation loops on the same header exclude each other.
uint64_t* SListWord(uint8_t* base, uint32_t header, const char* import)
{
    if (header == 0 || header % 8 != 0)
        Stop(StopCode::Unimplemented, "%s(%08X): a null or unaligned list header", import, header);
    return reinterpret_cast<uint64_t*>(base + header);
}
}  // namespace

GUEST_IMPORT(InterlockedPopEntrySList, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc:1549-1575 (atomic pop: next = first->next, depth - 1, sequence kept)")
{
    // (header*): the first entry, or 0 for an empty list.
    uint64_t* word = SListWord(base, ctx.r3.u32, "InterlockedPopEntrySList");
    for (;;)
    {
        const uint64_t raw = __atomic_load_n(word, __ATOMIC_SEQ_CST), value = __builtin_bswap64(raw);
        const uint32_t first = uint32_t(value >> 32);
        if (first == 0)
            return (void)(ctx.r3.u64 = 0);
        const uint16_t depth = uint16_t(value >> 16), sequence = uint16_t(value);
        const uint64_t next = runtime::Load32(base, first);
        const uint64_t updated = next << 32 | uint64_t(uint16_t(depth - 1)) << 16 | sequence;
        if (__sync_bool_compare_and_swap(word, raw, __builtin_bswap64(updated)))
            return (void)(ctx.r3.u64 = first);
    }
}

GUEST_IMPORT(InterlockedFlushSList, "REFERENCE_DERIVED: Xenia xboxkrnl_threading.cc:1577-1596 (atomic: the whole list taken, header zeroed)")
{
    // (header*): the first entry (0 if empty); the header becomes {0, 0, 0} (Xenia zeroes the sequence too).
    uint64_t* word = SListWord(base, ctx.r3.u32, "InterlockedFlushSList");
    const uint64_t raw = __atomic_exchange_n(word, 0, __ATOMIC_SEQ_CST);
    ctx.r3.u64 = uint32_t(__builtin_bswap64(raw) >> 32);
}

// FM4 Recomp: reached by SpeechFacade_default.xex after a race.
GUEST_IMPORT(KeInitializeSemaphore, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_threading.cc:639-653 (type 5, signal state = count, limit +0x10); wait list head from NT (INFERRED)")
{
    // (semaphore*, count, limit): the 20-byte KSEMAPHORE NtCreateSemaphore also builds (dispatcher header type 5, signal state
    // = count, limit at +0x10), the wait list head pointing at itself (+8, +0xC: an empty NT list, as KeInitializeMutant).
    // Xenia does not check the arguments; a null or misaligned pointer stops here.
    using namespace runtime;
    const uint32_t semaphore = ctx.r3.u32, count = ctx.r4.u32, limit = ctx.r5.u32;
    if (semaphore == 0 || semaphore % 4)
        Stop(StopCode::GuestFault, "KeInitializeSemaphore(%08X): null or misaligned (lr=%08X)", semaphore, uint32_t(ctx.lr));
    std::memset(base + semaphore, 0, 20);
    base[semaphore] = dispatch::kSemaphore;
    Store32(base, semaphore + dispatch::kSignalState, count);
    Store32(base, semaphore + 8, semaphore + 8);
    Store32(base, semaphore + 0xC, semaphore + 8);
    Store32(base, semaphore + dispatch::kSemaphoreLimit, limit);
}

GUEST_IMPORT(KeReleaseSemaphore, "REFERENCE_DERIVED: Xenia 67f16c4 xboxkrnl_threading.cc:655-674 (releases `adjustment`, returns the previous count); limit from NT (INFERRED)")
{
    // (semaphore*, increment, adjustment, wait): adds `adjustment` to the count and returns the count before. The priority
    // increment has no effect and `wait` needs nothing (Xenia's TODOs, as KeSetEvent here). Passing the limit makes NT raise
    // STATUS_SEMAPHORE_LIMIT_EXCEEDED, an exception, which is not modelled: it stops; so does an object that is not a semaphore.
    using namespace runtime;
    const uint32_t semaphore = ctx.r3.u32, adjustment = ctx.r5.u32;
    if (semaphore == 0 || semaphore % 4 || base[semaphore] != dispatch::kSemaphore)
        Stop(StopCode::GuestFault, "KeReleaseSemaphore(%08X): not a semaphore (lr=%08X)", semaphore, uint32_t(ctx.lr));
    int32_t previous = 0;
    if (!ReleaseSemaphore(base, semaphore, int32_t(adjustment), previous))
        Stop(StopCode::Unimplemented, "KeReleaseSemaphore(%08X, %u): past the limit %u (NT raises STATUS_SEMAPHORE_LIMIT_EXCEEDED) (lr=%08X)", semaphore,
             adjustment, Load32(base, semaphore + dispatch::kSemaphoreLimit), uint32_t(ctx.lr));
    ctx.r3.u64 = uint32_t(previous);
}
