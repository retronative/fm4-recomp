// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_xmsg.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/runtime/test_runtime.cpp). FM4 Recomp changes: XMsgStartIORequestEx and the XMP messages only
// (XMsgInProcessCall: XMP and the XLiveBase logon id only); the current thread read from the PCR here; Xenia line numbers.
//
// XAM app messages (XMsgStartIORequestEx, XMsgInProcessCall) and the music player app (XMP, app 0xFA) on a console with no custom
// soundtrack playing: the title's own music stays in control. REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// kernel/xam/xam_msg.cc:51-79, apps/xmp_app.{h,cc}: 0x0007001A :391-410, 0x0007001B :412-436), re-derived. Only the
// messages FM4 has been seen to send are implemented; any other app or message, and asynchronous requests (an
// XOVERLAPPED), stop.
#include "imports.h"
#include "content.h"
#include "threads.h"
#include "objects.h"
#include "notify.h"
#include "overlapped.h"
#include "thread_state.h"
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace {
constexpr uint32_t kXmpApp = 0xFA, kSetPlaybackController = 0x0007001A, kGetPlaybackController = 0x0007001B;
constexpr uint32_t kPlaybackControllerChanged = 0x0A000003;  // XN_XMP_PLAYBACKCONTROLLERCHANGED (xmp_app.h:97)
enum class PlaybackClient : uint32_t { kSystem = 0, kTitle = 1 };
std::mutex xmpLock;
PlaybackClient playbackClient = PlaybackClient::kTitle;  // Xenia's start: the title plays its own music (xmp_app.cc:24)

// An XMP message, answered at once; the result is its HRESULT. Others stop.
//  - 0x0007001A XMPSetPlaybackController: a 12-byte {XMP client, controller, playback client}: the pairs 2/0 or 0/1
//    (Xenia asserts those), or 2/4, which FM4 sends (ForzaRecomp OBSERVED, after its intro video): 4 = XUSER_MAX_COUNT,
//    read as "no particular user" (INFERRED; Xenia and ReCarbon ignore the controller field, and nothing here depends
//    on it); the playback client (0 the system's player, 1 the title) is recorded. When it changes,
//    XN_XMP_PLAYBACKCONTROLLERCHANGED is queued with !client (Xenia queues it on every call; here only for a real
//    change: INFERRED). No soundtrack plays, so nothing else changes.
//  - 0x0007001B XMPGetPlaybackController: a 12-byte {XMP client 2, controller*, locked*}: controller 0 and not
//    locked (Xenia always answers 0 and 0).
uint32_t XmpMessage(uint8_t* base, uint32_t message, uint32_t buffer, uint32_t length)
{
    using runtime::Load32, runtime::Store32;
    if (message == kSetPlaybackController)
    {
        if (length != 12 || buffer == 0)
            Stop(StopCode::Unimplemented, "XMPSetPlaybackController with %u bytes at %08X is not modelled", length, buffer);
        const uint32_t xmpClient = Load32(base, buffer), controller = Load32(base, buffer + 4), client = Load32(base, buffer + 8);
        if (!((xmpClient == 2 && (controller == 0 || controller == 4)) || (xmpClient == 0 && controller == 1)) || client > 1)
            Stop(StopCode::Unimplemented, "XMPSetPlaybackController(%u, %u, %u) is not modelled", xmpClient, controller, client);
        bool changed;
        {
            std::lock_guard guard(xmpLock);
            changed = playbackClient != PlaybackClient(client);
            playbackClient = PlaybackClient(client);
        }
        if (changed)
            runtime::EnqueueNotification(base, kPlaybackControllerChanged, client ? 0 : 1);
        return 0;
    }
    if (message == kGetPlaybackController)
    {
        if ((length != 0 && length != 12) || buffer == 0)
            Stop(StopCode::Unimplemented, "XMPGetPlaybackController with %u bytes at %08X is not modelled", length, buffer);
        const uint32_t xmpClient = Load32(base, buffer), controllerOut = Load32(base, buffer + 4), lockedOut = Load32(base, buffer + 8);
        if (xmpClient != 2 || !controllerOut || !lockedOut)
            Stop(StopCode::Unimplemented, "XMPGetPlaybackController(%u, %08X, %08X) is not modelled", xmpClient, controllerOut, lockedOut);
        Store32(base, controllerOut, 0);
        Store32(base, lockedOut, 0);
        return 0;
    }
    Stop(StopCode::Unimplemented, "XMP message %08X is not implemented", message);
}
}  // namespace

namespace {
// XMsgStartIORequest[Ex] (Xenia xam_msg.cc:51-87: the same dispatch; Ex adds an argument nothing reads): a message to a
// XAM app, answered at once. XMP (app 0xFA): XmpMessage. XAM (app 0xFE) 0002A005: a Kinect (NUI) request, from
// FM4's NUI code: no Kinect is connected, so it fails with E_FAIL (Xenia's answer to any XAM message it lacks,
// apps/xam_app.cc:111-116), nothing written; FM4's callers turn a failure into their own E_FAIL and take their error
// path (VERIFIED in our translation). On success the thread's last error is 0 (Xenia :66-68). Any other app or
// message, and an XOVERLAPPED, stops (7).
// XGI (app 0xFB; Xenia apps/xgi_app.cc:25-58): the user's rich-presence contexts and properties as the title sets them.
// They are kept (per user, by id), not sent anywhere: offline there is no LIVE presence service to read them.
constexpr uint32_t kXgiApp = 0xFB, kXgiSetContext = 0x000B0006, kXgiSetProperty = 0x000B0007;
std::mutex xgiLock;
std::map<std::pair<uint32_t, uint32_t>, uint32_t> xgiContexts;                  // (user, context id) -> value
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> xgiProperties;    // (user, property id) -> value bytes

uint32_t XgiMessage(const uint8_t* base, uint32_t message, uint32_t buffer, uint32_t length)
{
    using namespace runtime;
    // XGIUserSetContextEx: {user index, unused, 0 (64 bits), context id +16, value +20}, 24 bytes (or length 0, Xenia).
    // XGIUserSetPropertyEx: {user index, ..., property id +16, value size +20, value* +24}. User index 4 or more, a
    // wrong length or a property over 4 KB stops (7).
    const uint32_t user = Load32(base, buffer);
    if (user >= 4)
        Stop(StopCode::Unimplemented, "XGI message %08X for user index %u", message, user);
    std::lock_guard guard(xgiLock);
    if (message == kXgiSetContext)
    {
        if (length != 0 && length != 24)
            Stop(StopCode::Unimplemented, "XGIUserSetContextEx with a %u-byte record", length);
        const uint32_t id = Load32(base, buffer + 16), value = Load32(base, buffer + 20);
        const auto found = xgiContexts.find({user, id});
        const bool changed = found == xgiContexts.end() || found->second != value;
        xgiContexts[{user, id}] = value;
        if (changed)
            std::fprintf(stderr, "[xam] XGIUserSetContextEx(user %u, context %08X, value %u): kept (offline: no presence service)\n", user, id, value);
        return 0;
    }
    const uint32_t id = Load32(base, buffer + 16), size = Load32(base, buffer + 20), value = Load32(base, buffer + 24);
    if (size > 4096 || (size && !value))
        Stop(StopCode::Unimplemented, "XGIUserSetPropertyEx(user %u, property %08X): %u bytes at %08X", user, id, size, value);
    xgiProperties[{user, id}].assign(base + value, base + value + size);
    std::fprintf(stderr, "[xam] XGIUserSetPropertyEx(user %u, property %08X, %u bytes): kept (offline: no presence service)\n", user, id, size);
    return 0;
}

void StartIORequest(PPCContext& ctx, uint8_t* base, const char* import)
{
    // Xenia xam_msg.cc:51-70: the app's answer; with an XOVERLAPPED (XGI messages only so far; others stop) it is completed
    // at once (CompleteOverlappedImmediate) and the call returns ERROR_IO_PENDING; the last error is cleared on success or IO_PENDING.
    using namespace runtime;
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, overlapped = ctx.r5.u32, buffer = ctx.r6.u32, length = ctx.r7.u32;
    const bool xgi = app == kXgiApp && (message == kXgiSetContext || message == kXgiSetProperty);
    if (overlapped && !xgi)  // only the XGI messages have been seen with one
        Stop(StopCode::Unimplemented, "%s(app %X, message %08X) with an XOVERLAPPED is not implemented", import, app, message);
    uint32_t result;
    if (app == 0xFE && message >= 0x0002A005 && message <= 0x0002A008)
        result = (1u << 31) | 0x4005;  // E_FAIL
    else if (xgi)
        result = XgiMessage(base, message, buffer, length);
    else if (app == kXmpApp)
        result = XmpMessage(base, message, buffer, length);
    else
        Stop(StopCode::Unimplemented, "%s: app %X message %08X is not implemented", import, app, message);
    if (overlapped)
    {
        CompleteOverlappedImmediate(ctx, base, overlapped, result, import);
        result = 0x3E5;  // ERROR_IO_PENDING
    }
    if (result == 0 || result == 0x3E5)
        Store32(base, Load32(base, ctx.r13.u32 + pcr::kCurrentThread) + kthread::kLastError, 0);
    ctx.r3.u64 = result;
}
}  // namespace

GUEST_IMPORT(XMsgStartIORequestEx, "REFERENCE_DERIVED: Xenia xam_msg.cc:51-79, apps/xmp_app.cc (XMP: no custom soundtrack)")
{
    // (app, message, XOVERLAPPED*, buffer, length, unknown*): StartIORequest.
    StartIORequest(ctx, base, "XMsgStartIORequestEx");
}

GUEST_IMPORT(XMsgStartIORequest, "REFERENCE_DERIVED: Xenia xam_msg.cc:81-87 (as XMsgStartIORequestEx)")
{
    // (app, message, XOVERLAPPED*, buffer, length): StartIORequest.
    StartIORequest(ctx, base, "XMsgStartIORequest");
}

GUEST_IMPORT(XMsgInProcessCall, "REFERENCE_DERIVED: Xenia xam_msg.cc XMsgInProcessCall (dispatch); INFERRED: an offline console has no LIVE logon (id 0)")
{
    // (app, message, argument 1, argument 2): a synchronous message to a XAM app. XMP (0xFA): as XMsgStartIORequestEx.
    // XLiveBase (0xFC) 0x00058004 (Xenia: "XLiveBaseGetLogonId"): argument 1 receives a 32-bit logon id, argument 2
    // is its size (0 or 4). The profile is local and offline (a compatibility policy; no network): there is no LIVE logon,
    // so id 0 and success. Xenia stores 1, marked "?". FM4's only caller uses the id when the call succeeds and 0
    // when it fails (sign of the result minus one, ANDed with the id: VERIFIED in our translation),
    // so 0 is also what any failure would give it. Any other app or message stops (7).
    using namespace runtime;
    const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, out = ctx.r5.u32, length = ctx.r6.u32;
    if (app == kXmpApp)
    {
        ctx.r3.u64 = XmpMessage(base, message, out, length);
        return;
    }
    if (app == 0xFC && message == 0x00058046)
    {
        // Compatibility exception, for this
        // message only: success, the buffer untouched. Xenia xlivebase_app.cc:68-75: "Required to be successful for
        // 4D530910 [FM4] to detect signed-in profile ... doesn't seem to set anything in the given buffer". FM4's only
        // caller reads nothing from the buffer afterwards: success frees its allocation and returns, failure returns
        // ERROR_FUNCTION_FAILED (VERIFIED in our translation). This is local offline profile compatibility, not Xbox
        // LIVE connectivity; every other XLiveBase message stays unsupported. Logged: the first 3 calls, then every 100th.
        static std::atomic<uint32_t> calls{0};
        const uint32_t n = ++calls;
        if (n <= 3 || n % 100 == 0)
            std::fprintf(stderr, "[xam] XLiveBase 0x58046(%08X, %08X): compatibility success, buffer untouched (call %u)\n", out, length, n);
        ctx.r3.u64 = 0;
        return;
    }
    if (app == 0xFE && message == 0x0002000E)
    {
        // XamAppEnumerateContentAggregate (Xenia apps/xam_app.cc:29-71): FM4's aggregate content enumerator sends it from its
        // XamTaskSchedule work routine with the enumerator's own app id and message (VERIFIED in our translation).
        ctx.r3.u64 = EnumerateContentAggregateMessage(base, out);
        return;
    }
    if (app == 0xFE && message == 0x0002C009)
    {
        // XAM (app 0xFE) message 0002C009: unknown to Xenia (E_FAIL for any message it lacks). FM4's NUI code sends a
        // 4-byte record holding 0 or a guest function (a callback to register); both callers discard the result
        // (VERIFIED in our translation). No Kinect events exist to call it with: E_FAIL, nothing registered; only this message.
        ctx.r3.u64 = (1u << 31) | 0x4005;  // E_FAIL
        return;
    }
    if (app == 0xFE && message == 0x0002C00C)
    {
        // XAM (app 0xFE) message 0002C00C: unknown to Xenia (E_FAIL for any message it lacks, apps/xam_app.cc:111-116).
        // FM4 uses it to read Kinect (NUI) control settings: a 24-byte record {2, 257, "NuiV1Ctrl", key, buffer*, size*}
        // whose 120-byte answer it parses as numbers; on failure (or an unparsable answer) it uses built-in defaults such
        // as "1.5,3,53.0,0.19,0.43,50,0.5,0.5" (VERIFIED in our translation). No Kinect and no stored NUI settings exist:
        // E_FAIL, nothing written; only this message.
        ctx.r3.u64 = (1u << 31) | 0x4005;  // E_FAIL
        return;
    }
    if (app == 0xFE && message == 0x0002B004)
    {
        // XAM (app 0xFE) message 0002B004: unknown to Xenia (E_FAIL for any message it lacks). FM4 sends it with no
        // arguments right after XamXStudioRequest(770, 0), the counterpart of its start-up 0002B003; both callers discard
        // the result (VERIFIED in our translation). Unsupported: E_FAIL; only this message.
        ctx.r3.u64 = (1u << 31) | 0x4005;  // E_FAIL
        return;
    }
    if (app == 0xFE && message == 0x0002B003)
    {
        // XAM (app 0xFE) message 0002B003: unknown to Xenia, whose XAM app answers any message it does not implement
        // with E_FAIL (apps/xam_app.cc:111-116). FM4 sends it once at start-up with a 24-byte record {5, 0x511C, value}
        // and discards the result (VERIFIED in our translation). Unsupported here too: E_FAIL, nothing written; only
        // this message (any other XAM message stops).
        ctx.r3.u64 = (1u << 31) | 0x4005;  // E_FAIL
        return;
    }
    if (app == 0xFC && message == 0x00058020)
    {
        // CXLiveFriends::Enumerate (Xenia xlivebase_app.cc:41-49): a local offline profile has no LIVE friends list, so
        // the request fails with E_FAIL, Xenia's answer; nothing is written. FM4's caller frees its request and returns
        // the converted error (VERIFIED in our translation: the same shape as its 0x58046 call). A failure, not a
        // fabricated success; only this message.
        ctx.r3.u64 = (1u << 31) | 0x4005;  // E_FAIL
        return;
    }
    if (app != 0xFC || message != 0x00058004)
        Stop(StopCode::Unimplemented, "XMsgInProcessCall: app %X message %08X is not implemented", app, message);
    if (out == 0 || (length != 0 && length != 4))
        Stop(StopCode::Unimplemented, "XLiveBase logon id into %08X (%u bytes) is not modelled", out, length);
    Store32(base, out, 0);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XMsgCancelIORequest, "REFERENCE_DERIVED: Xenia xam_msg.cc:89-101 (returns 0; waits on the request's event if asked)")
{
    // (XOVERLAPPED*, wait): cancels an asynchronous message request. Every asynchronous request of this runtime completes
    // before its import returns (overlapped.h FinishOverlapped), so the request is already complete and its event already
    // set: nothing is left to cancel and the wait Xenia performs returns at once: 0. A request still marked pending
    // (its result field ERROR_IO_PENDING) would contradict that model: it stops rather than being dropped.
    const uint32_t overlapped = ctx.r3.u32;
    if (overlapped != 0 && runtime::Load32(base, overlapped) == runtime::overlapped::kErrorIoPending)
        Stop(StopCode::Unimplemented, "XMsgCancelIORequest(%08X, wait %u): the request is still pending, which this runtime never leaves",
             overlapped, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamTaskSchedule, "REFERENCE_DERIVED: Xenia xam_task.cc:48-86 (the callback on a new guest thread with the task message)")
{
    // (callback, message*, optional args*, handle*): a guest thread running callback(message), the executable's default
    // stack (at least 16 KB, rounded to 4 KB, as Xenia), started at once; handle* receives a thread handle (Xenia writes
    // a placeholder 12345). Optional args (Xenia only logs them) and a null handle pointer stop (7).
    using namespace runtime;
    const uint32_t callback = ctx.r3.u32, message = ctx.r4.u32, optional = ctx.r5.u32, handleOut = ctx.r6.u32;
    if (optional || !handleOut || !callback)
        Stop(StopCode::Unimplemented, "XamTaskSchedule(callback %08X, message %08X, args %08X, handle* %08X): not modelled", callback, message,
             optional, handleOut);
    uint32_t threadId = 0;
    const uint32_t kthread = CreateGuestThread(base, 0, 0, callback, message, 0, threadId);
    Store32(base, handleOut, CreateObject(ObjectType::Thread, kthread, ""));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XMsgCompleteIORequest, "REFERENCE_DERIVED: Xenia xam_msg.cc:104-111 (CompleteOverlappedImmediateEx)")
{
    // (XOVERLAPPED*, result, extended error, length): completes it with exactly these values and sets its event
    // (overlapped.h CompleteOverlappedEx); 0.
    runtime::CompleteOverlappedEx(ctx, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, "XMsgCompleteIORequest");
    ctx.r3.u64 = 0;
}
