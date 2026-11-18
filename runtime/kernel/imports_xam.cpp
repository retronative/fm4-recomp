// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XAM imports, added as runs reach them (FM4's order). Values and semantics: Xenia xenia-canary 67f16c4e315f
// (src/xenia/kernel/xam/), cited per import. ForzaRecomp's
// console_config.h (its choices for region, language, system version) is not taken.
#include "imports.h"
#include "keyboard_ui.h"
#include "loader.h"
#include "module.h"
#include "overlapped.h"
#include "user.h"
#include <cstring>

GUEST_IMPORT(XGetAVPack, "REFERENCE_DERIVED: Xenia xam_info.cc:205 XGetAVPack returns cvar avpack, default 8 (xam_info.cc:32)")
{
    // The AV pack in use. Xenia notes games use it as a PAL check and accept 3, 4, 6 or 8; 8 is its default.
    ctx.r3.u64 = 8;
}

// Launch data: the runtime always starts the title cold (no other title launched it with data), so
// there is none (Xenia: loader_data.launch_data_present false).
GUEST_IMPORT(XamLoaderGetLaunchDataSize, "REFERENCE_DERIVED: Xenia xam_info.cc:294 XamLoaderGetLaunchDataSize (no launch data)")
{
    // (size*): ERROR_INVALID_PARAMETER (0x57) without a pointer; else size 0 and ERROR_NOT_FOUND (0x490).
    if (ctx.r3.u32 == 0)
    {
        ctx.r3.u64 = 0x57;
        return;
    }
    runtime::Store32(base, ctx.r3.u32, 0);
    ctx.r3.u64 = 0x490;
}

GUEST_IMPORT(XamLoaderGetLaunchData, "REFERENCE_DERIVED: Xenia xam_info.cc:311 XamLoaderGetLaunchData (no launch data)")
{
    // (buffer*, size): nothing copied, ERROR_NOT_FOUND (0x490).
    (void)base;
    ctx.r3.u64 = 0x490;
}

GUEST_IMPORT(XGetGameRegion, "REFERENCE_DERIVED: Xenia xam_info.cc:217 xeXGetGameRegion (table by user country)")
{
    // Xenia maps the user's country to a region through its table; the runtime's country is 103 (US, the
    // XConfig policy in imports_xconfig.cpp), and table[103] (xam_info.cc:231, the 8th value of the 13th
    // row) is 0x00FF (North America).
    (void)base;
    ctx.r3.u64 = 0x00FF;
}

GUEST_IMPORT(XamGetSystemVersion, "REFERENCE_DERIVED: Xenia xam_info.cc:190 (format from its example values); version policy 2.0.17559")
{
    // The system version, consistent with the XboxKrnlVersion policy 2.0.17559 (module.cpp): major << 28 |
    // minor << 24 | build << 8 | QFE = 2 << 28 | 0 | 0x4497 << 8 | 0 = 0x20449700. The format is INFERRED from Xenia's
    // example values 0x200A3200 (2.0.2610.0) and 0x20096B00 (2.0.2411.0); Xenia itself returns 0 ("pretend to be old").
    (void)base;
    ctx.r3.u64 = 0x20449700;
}

GUEST_IMPORT(XamUserReadProfileSettings, "REFERENCE_DERIVED: Xenia xam_user.cc:159-339 XamUserReadProfileSettingsEx, kernel_state.h:122, user_profile.{h,cc}")
{
    // (title, user index, xuid count, xuids*, setting count, setting ids*, buffer size*, buffer*, overlapped*), in
    // Xenia's order: 1-32 settings, a buffer size pointer, and a buffer when the size is non-zero, else
    // X_ERROR_INVALID_PARAMETER (0x57); the size needed (8 + 40 per setting + the size field of each string or binary
    // setting key: id 14 bits, 2, size 12, type 4, types 4 and 6) written when the given size is 0, and
    // X_ERROR_INSUFFICIENT_BUFFER (0x7A) while the buffer is missing or short (returned directly, overlapped or not);
    // then a user index that is not signed in (user.h: only user 0) is X_ERROR_NO_SUCH_USER (0x525); user 0, by index
    // or by its one XUID, gets its profile's settings (profile_settings.cpp) and 0. With an XOVERLAPPED, that result
    // completes it (Xenia CompleteOverlappedImmediate, kernel/overlapped.h) and the call returns ERROR_IO_PENDING.
    // Stops (7): several XUIDs or another XUID, a setting the profile lacks, an XOVERLAPPED with a completion routine.
    const uint32_t userIndex = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32, count = ctx.r7.u32, ids = ctx.r8.u32;
    const uint32_t sizeOut = ctx.r9.u32, buffer = ctx.r10.u32, overlapped = runtime::Argument(ctx, base, 8);
    if (xuidCount != 0 && (xuidCount != 1 || xuids == 0 ||
                           (uint64_t(runtime::Load32(base, xuids)) << 32 | runtime::Load32(base, xuids + 4)) != runtime::user::kXuid))
        Stop(StopCode::Unimplemented, "XamUserReadProfileSettings: an XUID lookup (%u XUIDs at %08X) other than user 0's is not modelled",
             xuidCount, xuids);
    if (count < 1 || count > 32 || sizeOut == 0)
        return (void)(ctx.r3.u64 = 0x57);
    const uint32_t size = runtime::Load32(base, sizeOut);
    if (size != 0 && buffer == 0)
        return (void)(ctx.r3.u64 = 0x57);
    uint32_t needed = 8 + 40 * count;  // one XUID at most: Xenia's per-XUID multiplication is by 1
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t key = runtime::Load32(base, ids + 4 * i), type = key >> 28;
        if (type == 4 || type == 6)
            needed += (key >> 16) & 0xFFF;
    }
    if (buffer == 0 || size < needed)
    {
        if (size == 0)
            runtime::Store32(base, sizeOut, needed);
        return (void)(ctx.r3.u64 = 0x7A);
    }
    if (overlapped)
        runtime::CheckOverlapped(ctx, base, overlapped, "XamUserReadProfileSettings");  // before anything is written
    uint32_t result = 0x525;  // X_ERROR_NO_SUCH_USER
    if (xuidCount != 0 || runtime::user::SignedIn(userIndex))
    {
        runtime::user::WriteProfileSettings(base, buffer, count, ids, xuidCount != 0, userIndex);
        result = 0;
    }
    if (overlapped)
    {
        runtime::CompleteOverlappedImmediate(ctx, base, overlapped, result, "XamUserReadProfileSettings");
        result = runtime::overlapped::kErrorIoPending;
    }
    ctx.r3.u64 = result;
}

GUEST_IMPORT(XamVoiceIsActiveProcess, "REFERENCE_DERIVED: Xenia xam_voice.cc:20-23 (0)")
{
    // (): 0, no voice session: no headset or voice chat is modelled (offline console, project policy).
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamVoiceCreate, "REFERENCE_DERIVED: Xenia xam_voice.cc:26-32 (output zeroed, X_ERROR_ACCESS_DENIED); owner policy: voice absent")
{
    // (user index, flags (FM4: 0xF), DWORD* voice): no voice device exists here (the "honest absent answers" policy for
    // voice): the voice handle is zeroed and the call fails with ERROR_ACCESS_DENIED (5), as Xenia. A null output pointer
    // stops (7) (Xenia writes through it).
    const uint32_t out = ctx.r5.u32;
    if (out == 0)
        Stop(StopCode::Unimplemented, "XamVoiceCreate(user %u, flags %X) without an output pointer is not modelled", ctx.r3.u32, ctx.r4.u32);
    runtime::Store32(base, out, 0);
    ctx.r3.u64 = 5;  // X_ERROR_ACCESS_DENIED
}

GUEST_IMPORT(XamVoiceSetMicArrayIdleUsers, "APPROXIMATE: Xenia lists the export only (xam_table.inc:960); no Kinect is connected; FM4 ignores the result (VERIFIED)")
{
    // (?, ?, ?): which users the Kinect microphone array treats as idle (semantics UNKNOWN). No Kinect is
    // connected, so there is nothing to set: 0. FM4 reaches it through XexGetProcedureAddress (ordinal 0x48C) from
    // one wrapper whose three callers never read the result (VERIFIED in our translation).
    // A Kinect model would replace this.
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetDeviceContext, "REFERENCE_DERIVED: Xenia xam_input.cc:221-235")
{
    // (user index, unknown, context*): *context = 0, then for a signed-in user (user.h) or an index whose low byte is
    // FF, *context = the index and 0; otherwise X_E_DEVICE_NOT_CONNECTED (HRESULT of ERROR_DEVICE_NOT_CONNECTED, 0x48F).
    const uint32_t index = ctx.r3.u32, out = ctx.r5.u32;
    runtime::Store32(base, out, 0);
    if (runtime::user::SignedIn(index) || (index & 0xFF) == 0xFF)
    {
        runtime::Store32(base, out, index);
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = (1u << 31) | (7u << 16) | 0x48F;
}

GUEST_IMPORT(XamReadTileToTextureEx, "REFERENCE_DERIVED: Xenia xam_user.cc:761-780 XamReadTileToTexture (the buffer filled with 0xFF, an "
                                     "XOVERLAPPED completed at once); the argument layout VERIFIED from FM4's caller")
{
    // (tile type, title id, tile id, user index, size selector, gamer tile key*, buffer, stride, height, XOVERLAPPED*). FM4's
    // gamer-picture reader resolves this by ordinal (0x2FD) when the dashboard version allows, else it parses the tile key and
    // calls XamReadTileToTexture with the same buffer, stride, height and XOVERLAPPED (VERIFIED in our translation), so those four
    // are arguments 6-9 here. The offline profile has no gamer picture: as Xenia's XamReadTileToTexture (the Ex form is unlisted
    // there), the tile is white (0xFF) and the call succeeds, through the XOVERLAPPED when one is given. A tile is 64 x 64 ARGB at
    // most; no buffer, or a larger size, stops (7).
    const uint32_t buffer = runtime::Argument(ctx, base, 6), stride = runtime::Argument(ctx, base, 7), height = runtime::Argument(ctx, base, 8);
    const uint32_t overlapped = runtime::Argument(ctx, base, 9);
    const uint64_t bytes = uint64_t(stride) * height;
    if (buffer == 0 || bytes == 0 || bytes > 64 * 64 * 4)
        Stop(StopCode::Unimplemented, "XamReadTileToTextureEx(type %u, user %u): buffer %08X, stride %u, height %u is not modelled", ctx.r3.u32,
             ctx.r6.u32, buffer, stride, height);
    std::memset(base + buffer, 0xFF, size_t(bytes));
    uint32_t result = 0;
    if (overlapped != 0)
    {
        runtime::CompleteOverlappedImmediate(ctx, base, overlapped, 0, "XamReadTileToTextureEx");
        result = runtime::overlapped::kErrorIoPending;
    }
    ctx.r3.u64 = result;
}

GUEST_IMPORT(XamUserGetName, "REFERENCE_DERIVED: Xenia xam_user.cc:111-126, string_util.h:43-52 (copy_truncating)")
{
    // (user index, char buffer*, length): an index of 4 or more is X_E_INVALIDARG; a signed-in user (user.h) gets
    // the gamertag "User" copied truncating to min(length, 16) bytes with its terminator, 0; any other user
    // X_E_NO_SUCH_USER (HRESULT of 0x525). Nothing is written unless signed in.
    const uint32_t index = ctx.r3.u32, buffer = ctx.r4.u32, length = ctx.r5.u32;
    if (index >= 4)
        return (void)(ctx.r3.u64 = (1u << 31) | (7u << 16) | 0x57);
    if (!runtime::user::SignedIn(index))
        return (void)(ctx.r3.u64 = (1u << 31) | (7u << 16) | 0x525);
    const char name[] = "User";
    const uint32_t room = length < 16 ? length : 16;
    if (room)
    {
        const uint32_t copied = sizeof(name) - 1 < room - 1 ? uint32_t(sizeof(name) - 1) : room - 1;
        std::memcpy(base + buffer, name, copied);
        base[buffer + copied] = 0;
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetXUID, "REFERENCE_DERIVED: Xenia xam_user.cc:29-57")
{
    // (user index, type mask, xuid*): no pointer is X_E_INVALIDARG; otherwise *xuid is written: an index of 4 or more
    // X_E_INVALIDARG and 0; a signed-in user (user.h) whose profile type shares a bit with the mask its XUID and 0
    // (the profile is offline: type 1, so a mask asking only for online XUIDs (2, 4) finds none); else
    // X_E_NO_SUCH_USER and 0.
    const uint32_t index = ctx.r3.u32, mask = ctx.r4.u32, out = ctx.r5.u32;
    constexpr uint32_t kInvalidArg = (1u << 31) | (7u << 16) | 0x57, kNoSuchUser = (1u << 31) | (7u << 16) | 0x525;
    if (out == 0)
        return (void)(ctx.r3.u64 = kInvalidArg);
    uint64_t xuid = 0;
    uint32_t result = index >= 4 ? kInvalidArg : kNoSuchUser;
    if (index < 4 && runtime::user::SignedIn(index) && (runtime::user::kProfileType & mask))
        xuid = runtime::user::kXuid + index, result = 0;
    runtime::Store32(base, out, uint32_t(xuid >> 32));
    runtime::Store32(base, out + 4, uint32_t(xuid));
    ctx.r3.u64 = result;
}

GUEST_IMPORT(XamMediaVerificationCreate, "INFERRED: disc media verification is not supported (ERROR_NOT_SUPPORTED); Xenia lists the export only (xam_table.inc:1586)")
{
    // (...): starts verifying the disc's media. This runtime cannot verify Xbox disc media, and a verification that
    // passes would be invented: ERROR_NOT_SUPPORTED (50), nothing written. FM4's caller treats 1393 (ERROR_DISK_CORRUPT)
    // as a disc error, 0 as a verification to follow, and any other result by cleaning up and continuing (VERIFIED in
    // our translation).
    ctx.r3.u64 = 50;
}

GUEST_IMPORT(XamMediaVerificationClose, "INFERRED: no verification is ever created (XamMediaVerificationCreate fails); Xenia lists the export only (xam_table.inc:1587)")
{
    // (): ends the media verification: none exists here: 0.
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamNuiGetDeviceStatus, "REFERENCE_DERIVED: Xenia xam_nui.cc:28-41 (24 bytes, status +12: 0, not connected)")
{
    // (X_NUI_DEVICE_STATUS*): no Kinect is connected: the 24-byte record zeroed, status (+12) 0. No result. A null
    // pointer stops (7) (Xenia writes through it).
    const uint32_t status = ctx.r3.u32;
    if (status == 0)
        Stop(StopCode::Unimplemented, "XamNuiGetDeviceStatus without a status pointer is not modelled");
    std::memset(base + status, 0, 24);
}

GUEST_IMPORT(XamXStudioRequest, "INFERRED: no XStudio service exists here (E_NOTIMPL); Xenia lists the export only (xam_table.inc:1308)")
{
    // (request, out*, ...): a request to the XStudio service, which neither Xenia nor this runtime provides: the
    // service is absent, so E_NOTIMPL, nothing written. FM4's caller checks only the sign: on success it installs a
    // handler, on failure it skips that (VERIFIED in our translation).
    ctx.r3.u64 = (1u << 31) | 0x4001;  // E_NOTIMPL
}

GUEST_IMPORT(XamXlfsInitializeUploadQueue, "INFERRED: the LIVE file system's upload queue does not exist offline (E_FAIL); Xenia lists the export only")
{
    // (...): starts the queue that uploads to the LIVE file system, an online service: unavailable on this offline
    // console, so E_FAIL, nothing written. FM4 checks only the sign: on failure it records no queue and continues
    // (VERIFIED in our translation).
    ctx.r3.u64 = (1u << 31) | 0x4005;  // E_FAIL
}

GUEST_IMPORT(XamGetCurrentTitleId, "REFERENCE_DERIVED: Xenia xam_info.cc:257-260 (the running title's id)")
{
    // (): the executable's title id: its XEX header's execution info (optional header 0x00040006, title id at +0xC;
    // Xenia's emulator()->title_id() comes from the same field). An executable without execution info stops (7).
    const uint32_t info = runtime::XexHeaderField(base, runtime::ExecutableModule().header, 0x00040006);
    if (info == 0)
        Stop(StopCode::Unimplemented, "XamGetCurrentTitleId: the executable has no execution info");
    ctx.r3.u64 = runtime::Load32(base, info + 0xC);
}

GUEST_IMPORT(XamNuiIdentityGetSessionId, "HYPOTHESIS: no Kinect identity session (0); Xenia lists the export only")
{
    // (): the Kinect identity session's 64-bit id. No Kinect is connected (XamNuiGetDeviceStatus), so there is no
    // session: 0 (that 0 means "none" is a HYPOTHESIS). FM4 stores the value and moves on (OBSERVED in our translation).
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamNuiGetTrueColorInfo, "INFERRED: no Kinect colour camera (E_NOTIMPL, FM4's own answer when the export is missing); Xenia lists the export only")
{
    // (...): information about the Kinect's colour camera. No Kinect is connected. FM4 reaches it through a wrapper
    // that returns E_NOTIMPL itself when the export cannot be resolved (VERIFIED in our translation), so its callers
    // already handle that answer: E_NOTIMPL, nothing written.
    ctx.r3.u64 = (1u << 31) | 0x4001;  // E_NOTIMPL
}

GUEST_IMPORT(XamNuiCameraTiltSetCallback, "HYPOTHESIS: registers a Kinect tilt-motor callback; no Kinect, so it never fires; Xenia lists the export only")
{
    // (callback, ...): registers the function the Kinect's tilt motor reports to. No Kinect is connected, so the
    // callback is recorded and never called (as a listener that gets no events). The result is discarded by FM4's
    // only caller (VERIFIED in our translation); 0 (the result's type is a HYPOTHESIS). A callback that is not a
    // translated function stops (7).
    static uint32_t tiltCallback = 0;
    const uint32_t callback = ctx.r3.u32;
    if (callback != 0 && runtime::FindFunction(base, callback) == nullptr)
        Stop(StopCode::Unimplemented, "XamNuiCameraTiltSetCallback: %08X is no translated function", callback);
    tiltCallback = callback;
    (void)tiltCallback;
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamGetActiveDashAppInfo, "INFERRED: no dashboard app is active while a title runs (E_FAIL); Xenia lists the export only")
{
    // (info*, ...): describes the dashboard app that is active. None is: E_FAIL, nothing written. FM4 zeroes the record
    // first, discards the result and passes the (zero) fields to its event-trace producer (VERIFIED in our translation).
    ctx.r3.u64 = (1u << 31) | 0x4005;  // E_FAIL
}

GUEST_IMPORT(PsCamDeviceRequest, "INFERRED: no Xbox Live Vision camera (STATUS_DEVICE_NOT_CONNECTED); Xenia lists the export only (xboxkrnl_table.inc:795)")
{
    // (request*, ...): a request to the camera device. No camera is connected: STATUS_DEVICE_NOT_CONNECTED (an NT
    // status: INFERRED for this kernel call), nothing written. FM4 calls it through a pointer its XStudio set-up would
    // replace (XamXStudioRequest fails here) and skips the rest on any negative result (VERIFIED in our translation).
    ctx.r3.u64 = (3u << 30) | 0x9D;  // STATUS_DEVICE_NOT_CONNECTED
}

// FM4 Recomp: reached by SpeechFacade_default.xex.
GUEST_IMPORT(XamIsUIActive, "REFERENCE_DERIVED: Xenia 67f16c4 xam_ui.cc:207 (xeXamIsUIActive: a XAM dialog is shown)")
{
    // (): whether a system UI (guide, dialog) is open: the only one this runtime shows is XamShowKeyboardUI's (keyboard_ui.h).
    (void)base;
    ctx.r3.u64 = runtime::keyboard_ui::Active() ? 1 : 0;
}
