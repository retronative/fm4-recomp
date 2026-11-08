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
#include "file_system.h"
#include <cstdio>
#include "indirect.h"
#include <map>
#include <mutex>

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
    case ObjectType::Title: return 0;           // its own type: compared from its header (ObReferenceObjectByHandle)
    }
    return DataImportValue("ExEventObjectType");
}

// An object type the executable can hold: an imported kernel type export or a title type (ObCreateObject).
bool IsKnownObjectType(uint32_t typeAddress);
// A live title-type object (ObCreateObject) by its body: its type, or 0 once its last reference freed it.
uint32_t TitleObjectType(uint32_t body);
// One more reference to a live title-type object: its header's pointer count (+0) goes up.
void RetainTitleObject(uint8_t* base, uint32_t body);

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
    const uint32_t expected = object->type == ObjectType::Title ? TitleObjectType(object->header) : ObjectTypeAddress(object->type);
    if (object->type == ObjectType::Title && expected == 0)
        Stop(StopCode::GuestFault, "ObReferenceObjectByHandle(handle %08X, lr %08X): a handle to a title object (%08X) that was freed", handle,
             uint32_t(ctx.lr), object->header);
    // FM4 Recomp: the executable imports no type export for this object's type (a timer, mutant or XAM object), so the
    // caller cannot hold that type's address; a type it does hold (an imported kernel type or a title type) is another
    // type's, hence a mismatch (the kernel's types are distinct objects; Xenia xboxkrnl_ob.cc:123-127 compares the type
    // pointer the same way). Any other address cannot be told apart from the missing export and stops.
    if (typeAddress && expected == 0)
    {
        if (!IsKnownObjectType(typeAddress))
            Stop(StopCode::Unimplemented,
                 "ObReferenceObjectByHandle(handle %08X, type %08X, out %08X, lr %08X): an object of type %u (objects.h ObjectType), "
                 "for which the executable imports no type export, against a type address no import or title type has",
                 handle, typeAddress, out, uint32_t(ctx.lr), unsigned(object->type));
        ctx.r3.u64 = status::kObjectTypeMismatch;
        return;
    }
    if (typeAddress && typeAddress != expected)
    {
        ctx.r3.u64 = status::kObjectTypeMismatch;
        return;
    }
    // The caller takes a reference, released by ObDereferenceObject (Xenia xboxkrnl_ob.cc:131-133 RetainHandle). Kernel
    // objects live for the whole run (their references are not counted); a title-type object's are, since its last
    // dereference frees it (DereferenceTitleObject): FM4 references its NUI object by handle and dereferences it after,
    // and without this count the pair freed the object under its open handle.
    if (object->type == ObjectType::Title)
        RetainTitleObject(base, object->header);
    Store32(base, out, object->header);
    ctx.r3.u64 = status::kSuccess;
}

namespace {
// Objects of title-defined types (ObCreateObject): body -> the guest block (header) it lives in and its type. The
// header is Xenia's X_OBJECT_HEADER (xobject.h:66-85, 0x18 bytes before the body: pointer count +0, handle count +4,
// offsets and flags +8, create info +0xC, object type +0x10 (body - 8), +0x14); the type Xenia's X_OBJECT_TYPE with the
// procedures as the original Xbox kernel's OBJECT_TYPE (allocate +0, free +4, close +8, delete +0xC, parse +0x10,
// default object +0x14, pool tag +0x18; FM4's 'Nuio' type: the local capture menu5 (not distributed)).
constexpr uint32_t kObjectHeaderSize = 0x18;
struct TitleObject { uint32_t block, type; };
std::mutex titleObjectsLock;
std::map<uint32_t, TitleObject> titleObjects;
}  // namespace

bool runtime::IsKnownObjectType(uint32_t typeAddress)
{
    using namespace runtime;
    for (ObjectType t : {ObjectType::Event, ObjectType::Thread, ObjectType::Semaphore, ObjectType::File})
        if (ObjectTypeAddress(t) == typeAddress)
            return true;
    std::lock_guard guard(titleObjectsLock);
    for (const auto& [body, object] : titleObjects)
        if (object.type == typeAddress)
            return true;
    return false;
}

uint32_t runtime::TitleObjectType(uint32_t body)
{
    std::lock_guard guard(titleObjectsLock);
    const auto it = titleObjects.find(body);
    return it == titleObjects.end() ? 0 : it->second.type;
}

void runtime::RetainTitleObject(uint8_t* base, uint32_t body)
{
    std::lock_guard guard(titleObjectsLock);
    if (const auto it = titleObjects.find(body); it != titleObjects.end())
        Store32(base, it->second.block, Load32(base, it->second.block) + 1);
}

namespace {

// Calls a guest procedure of an object type with `r3` (and `r4`) on the calling thread's stack; returns r3.
uint32_t CallTypeProcedure(PPCContext& ctx, uint8_t* base, uint32_t procedure, uint32_t r3, uint32_t r4)
{
    PPCContext c = ctx;
    c.r3.u64 = r3, c.r4.u64 = r4;
    PpcCallIndirect(c, base, procedure);
    return c.r3.u32;
}

// Drops one reference to a title-type object; at zero the type's free procedure (+4) gets its block and it is forgotten
// (a delete procedure, +0xC, stops (7): none seen yet). False if `object` is not one.
bool DereferenceTitleObject(PPCContext& ctx, uint8_t* base, uint32_t object)
{
    using namespace runtime;
    TitleObject titled{};
    {
        std::lock_guard guard(titleObjectsLock);
        const auto it = titleObjects.find(object);
        if (it == titleObjects.end())
            return false;
        titled = it->second;
    }
    const uint32_t count = Load32(base, titled.block);
    if (count == 0)
        Stop(StopCode::GuestFault, "ObDereferenceObject(%08X): its pointer count is already 0", object);
    Store32(base, titled.block, count - 1);
    if (count > 1)
        return true;
    if (Load32(base, titled.type + 0xC))
        Stop(StopCode::Unimplemented, "ObDereferenceObject(%08X): the type's delete procedure is not modelled", object);
    {
        std::lock_guard guard(titleObjectsLock);
        titleObjects.erase(object);
    }
    if (const uint32_t free = Load32(base, titled.type + 4))
        CallTypeProcedure(ctx, base, free, titled.block, 0);
    return true;
}
}  // namespace

GUEST_IMPORT(ObCreateObject, "INFERRED: original Xbox kernel ObCreateObject (OBJECT_TYPE procedures); header layout Xenia xobject.h:66-85, xobject.cc:315-340")
{
    // (object type*, object attributes*, body size, object*): the type's allocate procedure (+0) is called with the header
    // and body size (0x18 + size) and the type's pool tag (+0x18); a null block is STATUS_INSUFFICIENT_RESOURCES
    // (C000009A) with object* = 0. Otherwise the block is zeroed, its header set (pointer count 1, handle count 0, type),
    // object* = the body (block + 0x18), 0. Xenia and ShiftGlue 72b7dd6f leave ObCreateObject a stub (ShiftGlue
    // xboxkrnl_ob.cpp:259 returns with r3 unchanged): not followed. Named objects (attributes != 0), a type without an
    // allocate procedure or a null object* stop (7). FM4 creates its NUI ('Nuio') objects this way (the local capture menu5 (not distributed)).
    using namespace runtime;
    const uint32_t type = ctx.r3.u32, attributes = ctx.r4.u32, size = ctx.r5.u32, out = ctx.r6.u32;
    const uint32_t allocate = type ? Load32(base, type) : 0;
    if (attributes || !allocate || !out || size > 0x100000)
        Stop(StopCode::Unimplemented, "ObCreateObject(type %08X, attributes %08X, size %u, object* %08X): not modelled", type, attributes, size, out);
    const uint32_t block = CallTypeProcedure(ctx, base, allocate, kObjectHeaderSize + size, Load32(base, type + 0x18));
    if (!block)
    {
        Store32(base, out, 0);
        return (void)(ctx.r3.u64 = 0xC000009A);  // STATUS_INSUFFICIENT_RESOURCES
    }
    std::memset(base + block, 0, kObjectHeaderSize + size);
    Store32(base, block, 1);
    Store32(base, block + 0x10, type);
    {
        std::lock_guard guard(titleObjectsLock);
        titleObjects[block + kObjectHeaderSize] = {block, type};
    }
    Store32(base, out, block + kObjectHeaderSize);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(ObOpenObjectByPointer, "REFERENCE_DERIVED: Xenia xboxkrnl_ob.cc:57-70 (a new handle to the object; UNSUCCESSFUL if unknown)")
{
    // (object*, handle*): handle* = 0, then a new handle to the object, 0. Here for objects of title-defined types
    // (ObCreateObject): the header's handle count (+4) and pointer count (+0) go up (a handle references its object; the
    // reference is dropped at NtClose). Another object: STATUS_UNSUCCESSFUL (C0000001) as Xenia for one it does not know,
    // except kernel objects with a body here, which stop (7) until a run needs them.
    using namespace runtime;
    const uint32_t object = ctx.r3.u32, out = ctx.r4.u32;
    if (out)
        Store32(base, out, 0);
    uint32_t block = 0;
    {
        std::lock_guard guard(titleObjectsLock);
        if (const auto it = titleObjects.find(object); it != titleObjects.end())
            block = it->second.block;
    }
    if (!block)
    {
        if (IsObjectBody(object))
            Stop(StopCode::Unimplemented, "ObOpenObjectByPointer(%08X): a kernel object: not modelled", object);
        return (void)(ctx.r3.u64 = 0xC0000001);
    }
    if (!out)
        Stop(StopCode::Unimplemented, "ObOpenObjectByPointer(%08X): a null handle pointer", object);
    Store32(base, block + 4, Load32(base, block + 4) + 1);
    Store32(base, block, Load32(base, block) + 1);
    Store32(base, out, CreateObject(ObjectType::Title, object, ""));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(ObDereferenceObject, "REFERENCE_DERIVED: Xenia xboxkrnl_ob.cc ObDereferenceObject; title-type objects: original Xbox kernel (OBJECT_TYPE free procedure)")
{
    // Kernel objects live for the whole run, so a dereference only checks its argument. An object ObCreateObject made
    // (a title-defined type): DereferenceTitleObject. A null or unknown object pointer stops (Xenia logs and ignores it).
    using namespace runtime;
    const uint32_t object = ctx.r3.u32;
    if (DereferenceTitleObject(ctx, base, object))
        return;
    ThreadState thread{};
    if (object == 0 || !(IsObjectBody(object) || FindThread(object, thread)))
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
    // Releases one handle; the object (in the kernel arena) lives on. A file handle marked delete-on-close
    // (NtSetInformationFile FileDispositionInformation) removes its saved-content host file now (NT; Xenia ignores the mark).
    const auto object = runtime::LookupHandle(ctx.r3.u32);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    if (object && object->type == runtime::ObjectType::Title)
    {
        // A handle to a title-type object (ObOpenObjectByPointer): the type's close procedure (+8) gets (object, handle
        // count before this close) (INFERRED from the original Xbox kernel; FM4's NUI close treats 1 as the last handle),
        // the handle count (+4) goes down, then the handle's reference is dropped (DereferenceTitleObject).
        uint32_t block = 0, type = 0;
        {
            std::lock_guard guard(titleObjectsLock);
            if (const auto it = titleObjects.find(object->header); it != titleObjects.end())
                block = it->second.block, type = it->second.type;
        }
        if (!block || !runtime::CloseHandle(ctx.r3.u32))
            Stop(StopCode::GuestFault, "NtClose(%08X): a title object handle whose object is gone", ctx.r3.u32);
        const uint32_t handles = runtime::Load32(base, block + 4);
        if (const uint32_t close = runtime::Load32(base, type + 8))
            CallTypeProcedure(ctx, base, close, object->header, handles);
        runtime::Store32(base, block + 4, handles ? handles - 1 : 0);
        DereferenceTitleObject(ctx, base, object->header);
        return (void)(ctx.r3.u64 = runtime::status::kSuccess);
    }
    const bool closed = runtime::CloseHandle(ctx.r3.u32);
    if (closed && file && file->deleteOnClose)
    {
        runtime::RemoveHostFile(file->entry);
        file->deleteOnClose = false;
        std::fprintf(stderr, "[fs] NtClose: %s deleted on close\n", file->entry.host.c_str());
    }
    ctx.r3.u64 = closed ? runtime::status::kSuccess : runtime::status::kInvalidHandle;
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
