// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_notify.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/runtime/test_runtime.cpp). FM4 Recomp changes: reduced to the two imports and the queue
// (no hang-report line or test reset yet); the broadcaster (EnqueueNotification) taken with XMP (imports_xmsg.cpp).
//
// XAM notification listeners. REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f: kernel/xam/xam_notify.cc:15-92,
// kernel/xnotifylistener.cc), re-derived. A listener is a kernel object whose body is a notification (manual-reset)
// event, signalled exactly while its queue holds a notification, so a title can wait on its handle. Difference, by
// design (compatibility policy): Xenia queues XN_SYS_UI on/off and XN_SYS_SIGNINCHANGED twice for the first listener
// whose mask has bit 0 (kernel_state.cc:731-742, "games seem to expect" them, a forum-sourced guess); here no
// notification is fabricated, so a new listener's queue is empty (as ForzaRecomp, which FM4 ran past).
#include "arena.h"
#include "dispatcher.h"
#include "imports.h"
#include "notify.h"
#include "objects.h"
#include <deque>
#include <map>
#include <mutex>
#include <utility>

namespace {
struct Listener
{
    uint64_t mask;
    uint32_t maxVersion;
    std::deque<std::pair<uint32_t, uint32_t>> queue;  // (id, data)
};
std::mutex listenersLock;
std::map<uint32_t, Listener> listeners;  // by object body (its event header)

// The listener a handle names, or null (an unknown handle, another object type, a closed one).
Listener* Find(uint32_t handle, uint32_t& body)
{
    const auto object = runtime::LookupHandle(handle);
    if (!object || object->type != runtime::ObjectType::NotifyListener)
        return nullptr;
    body = object->header;
    const auto it = listeners.find(body);
    return it == listeners.end() || !runtime::IsObjectBody(body) ? nullptr : &it->second;
}
constexpr uint32_t MaskIndex(uint32_t id) { return (id >> 25) & 0x3F; }  // XNotificationKey (xnotifylistener.h)
constexpr uint32_t Version(uint32_t id) { return (id >> 16) & 0x1FF; }
}  // namespace

void runtime::EnqueueNotification(uint8_t* base, uint32_t id, uint32_t data)
{
    std::lock_guard lock(listenersLock);
    for (auto& [body, listener] : listeners)
    {
        if (!IsObjectBody(body) || !(listener.mask >> MaskIndex(id) & 1) || Version(id) > listener.maxVersion)
            continue;  // closed, or the id is filtered out (Xenia xnotifylistener.cc EnqueueNotification)
        listener.queue.emplace_back(id, data);
        SetEvent(base, body);
    }
}

GUEST_IMPORT(XamNotifyCreateListener, "REFERENCE_DERIVED: Xenia xam_notify.cc:15-34 (no startup notifications: policy, see above)")
{
    // (mask, 64-bit; maximum version): a listener handle, its queue empty. Xenia asserts a version below 11; a larger
    // one is not modelled and stops.
    using namespace runtime;
    const uint64_t mask = ctx.r3.u64;
    const uint32_t maxVersion = ctx.r4.u32;
    if (maxVersion > 10)
        Stop(StopCode::Unimplemented, "XamNotifyCreateListener: maximum version %u is not modelled", maxVersion);
    const uint32_t body = ArenaAlloc(base, 16);  // a notification event, not signalled
    base[body] = dispatch::kNotificationEvent;
    Store32(base, body + dispatch::kSignalState, 0);
    {
        std::lock_guard lock(listenersLock);
        listeners[body] = {mask, maxVersion, {}};
    }
    ctx.r3.u64 = CreateObject(ObjectType::NotifyListener, body, "");
}

GUEST_IMPORT(XNotifyGetNext, "REFERENCE_DERIVED: Xenia xam_notify.cc:53-91")
{
    // (handle, id to match or 0 for any, id*, data*): 1 and the first queued notification (the first with that id),
    // removed; else 0, with id and data 0. Without an id pointer nothing is removed (0). The event is reset once the
    // queue is empty. A handle that is not an open listener stops (Xenia returns 0; here it would be a runtime fault,
    // not a title state).
    using namespace runtime;
    const uint32_t handle = ctx.r3.u32, match = ctx.r4.u32, idOut = ctx.r5.u32, dataOut = ctx.r6.u32;
    std::lock_guard lock(listenersLock);
    uint32_t body = 0;
    Listener* listener = Find(handle, body);
    if (!listener)
        Stop(StopCode::Unimplemented, "XNotifyGetNext(%08X): not an open notification listener", handle);
    if (idOut == 0)
    {
        if (dataOut)
            Store32(base, dataOut, 0);
        ctx.r3.u64 = 0;
        return;
    }
    auto it = listener->queue.begin();
    while (match && it != listener->queue.end() && it->first != match)
        ++it;
    const bool found = it != listener->queue.end();
    const uint32_t id = found ? it->first : 0, data = found ? it->second : 0;
    if (found)
    {
        listener->queue.erase(it);
        if (listener->queue.empty())
            ResetEvent(base, body);
    }
    if (dataOut)
        Store32(base, dataOut, data);
    Store32(base, idOut, id);
    ctx.r3.u64 = found ? 1 : 0;
}
