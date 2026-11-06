// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// XAM user queries over the user model (user.h: user 0 signed in locally with an offline profile, users 1-3 not
// signed in, nothing online). The profile is offline only, so XamUserGetSigninInfo withholds the XUID from a caller
// asking for the online XUID only (Xenia 838850fed838 xam_user.cc:129-131; 67f16c4 ignores the flags). Semantics:
// Xenia xenia-canary 67f16c4e315f src/xenia/kernel/xam/xam_user.cc wherever it has an implementation; three imports
// it lists only cite xenia-canary 838850fed838. XamUserGetAgeGroup has no source and keeps its
// strict stub. Any XUID other than user 0's, and every overlapped (asynchronous) form, stops.
#include "imports.h"
#include "arena.h"
#include "objects.h"
#include "user.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace {
constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorNoSuchUser = 0x525;
constexpr uint32_t kInvalidArg = (1u << 31) | (7u << 16) | 0x57, kNoSuchUser = (1u << 31) | (7u << 16) | 0x525;

void RequireUserXuid(const char* import, uint64_t xuid)
{
    if (xuid != runtime::user::kXuid)
        Stop(StopCode::Unimplemented, "%s(XUID %016llX): only user 0's XUID (%016llX) is modelled", import,
             (unsigned long long)xuid, (unsigned long long)runtime::user::kXuid);
}
}  // namespace

GUEST_IMPORT(XamUserGetSigninState, "REFERENCE_DERIVED: Xenia xam_user.cc:60-73, user_profile.h:229 (local offline profile)")
{
    // (user index): 1 (signed in locally) for user 0, else 0. The thread yields first (Xenia MaybeYield: titles poll it).
    std::this_thread::yield();
    ctx.r3.u64 = runtime::user::SignedIn(ctx.r3.u32) ? runtime::user::kSigninStateLocal : 0;
}

GUEST_IMPORT(XamUserGetSigninInfo, "REFERENCE_DERIVED: Xenia xam_user.cc:75-109; 838850fed838 xam_user.cc:129-131 (XUID by flags)")
{
    // (user index, flags, X_USER_SIGNIN_INFO*) -> HRESULT. The 40-byte record: XUID +0, sign-in state +0xC, name +0x18
    // (16 chars). No pointer: E_INVALIDARG; else zeroed; not signed in (any index but 0): HRESULT(NO_SUCH_USER); user 0:
    // state 1, "User", S_OK, and the XUID unless the flags ask for the online XUID only (1): the profile is offline.
    using namespace runtime;
    const uint32_t userIndex = ctx.r3.u32, flags = ctx.r4.u32, info = ctx.r5.u32;
    if (info == 0)
        return (void)(ctx.r3.u64 = kInvalidArg);
    std::memset(base + info, 0, 40);
    if (!user::SignedIn(userIndex))
        return (void)(ctx.r3.u64 = kNoSuchUser);
    if (flags == 0 || (flags & 2))
        Store32(base, info, uint32_t(user::kXuid >> 32)), Store32(base, info + 4, uint32_t(user::kXuid));
    Store32(base, info + 0xC, user::kSigninStateLocal);
    std::memcpy(base + info + 0x18, user::kGamertag, sizeof(user::kGamertag));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserCheckPrivilege, "REFERENCE_DERIVED: Xenia xam_user.cc:437-454 (every privilege denied)")
{
    // (user index, privilege, result*) -> Win32 error. Any user (0xFF) or user 0: denied (*result = 0), ERROR_SUCCESS:
    // a local offline profile holds no Live privilege. An index 4-0xFE: ERROR_INVALID_PARAMETER; users 1-3:
    // ERROR_NO_SUCH_USER; nothing written. A null result pointer stops (Xenia writes through it).
    using namespace runtime;
    const uint32_t userIndex = ctx.r3.u32, privilege = ctx.r4.u32, out = ctx.r5.u32;
    if (userIndex != 0xFF)
    {
        if (userIndex >= 4)
            return (void)(ctx.r3.u64 = kErrorInvalidParameter);
        if (!user::SignedIn(userIndex))
            return (void)(ctx.r3.u64 = kErrorNoSuchUser);
    }
    if (out == 0)
        Stop(StopCode::Unimplemented, "XamUserCheckPrivilege(user %X, privilege %X): no result pointer is not modelled", userIndex, privilege);
    Store32(base, out, 0);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetIndexFromXUID, "REFERENCE_DERIVED: Xenia 838850fed838 xam_user.cc:70-89 (none at 67f16c4); user 0's XUID only")
{
    // (xuid, 64-bit r3; flags; index*) -> HRESULT. No pointer: E_INVALIDARG. User 0's XUID: *index = 0, S_OK. Any other
    // XUID stops (838850f answers HRESULT(NO_SUCH_USER)).
    using namespace runtime;
    const uint64_t xuid = ctx.r3.u64;
    const uint32_t flags = ctx.r4.u32, out = ctx.r5.u32;
    if (out == 0)
        return (void)(ctx.r3.u64 = kInvalidArg);
    if (xuid != user::kXuid)
        Stop(StopCode::Unimplemented, "XamUserGetIndexFromXUID(XUID %016llX, flags %X): only user 0's XUID is modelled",
             (unsigned long long)xuid, flags);
    Store32(base, out, user::kSignedInIndex);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetMembershipTierFromXUID, "REFERENCE_DERIVED: Xenia 838850fed838 xam_user.cc:667-676, xam.h:44 (none at 67f16c4); offline: no tier")
{
    // (xuid) -> the Live tier (none 0, silver 3, gold 6, family gold 9). An offline profile has no Live account: 0.
    RequireUserXuid("XamUserGetMembershipTierFromXUID", ctx.r3.u64);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamUserGetOnlineCountryFromXUID, "REFERENCE_DERIVED: Xenia 838850fed838 xam_user.cc:1236-1245 (none at 67f16c4); XConfig fallback")
{
    // (xuid) -> a country code: the profile's online country, else XCONFIG_USER_COUNTRY. An offline profile has none,
    // so the console's country, 103 (US; the XConfig policy entry in imports_xconfig.cpp).
    RequireUserXuid("XamUserGetOnlineCountryFromXUID", ctx.r3.u64);
    ctx.r3.u64 = 103;
}

GUEST_IMPORT(XamUserAreUsersFriends, "REFERENCE_DERIVED: Xenia xam_user.cc:514-556 (no friends)")
{
    // (user index, xuids*, xuid count, are friends*, overlapped*) -> Win32 error. An index of 4 or more:
    // ERROR_INVALID_PARAMETER; users 1-3: ERROR_NO_SUCH_USER; user 0: ERROR_SUCCESS with no friends. The result is
    // written (0) in every case. The overlapped form, or no result pointer (Xenia asserts), stops.
    using namespace runtime;
    const uint32_t userIndex = ctx.r3.u32, xuids = ctx.r4.u32, xuidCount = ctx.r5.u32, out = ctx.r6.u32, overlapped = ctx.r7.u32;
    if (out == 0 || overlapped != 0)
        Stop(StopCode::Unimplemented, "XamUserAreUsersFriends(user %X, xuids %08X, %u XUIDs, result %08X, overlapped %08X): "
             "only the synchronous form is modelled", userIndex, xuids, xuidCount, out, overlapped);
    Store32(base, out, 0);
    ctx.r3.u64 = userIndex >= 4 ? kErrorInvalidParameter : user::SignedIn(userIndex) ? 0 : kErrorNoSuchUser;
}

// Profile enumeration (Xenia 838850fed838 xam_enum.cc:166-203, xenumerator.cc:227-249, xam.h:24-135 X_XAMACCOUNTINFO
// 0x17C bytes, :193-197 X_PROFILEENUMRESULT; 67f16c4 lists the exports only). The profiles stored on this console:
// the one signed-in local profile (user.h), on storage device 1 (the user-data device, kernel/content.h). Its account
// record: reserved flags 0 (not LIVE-enabled, no passcode), live flags 0, gamertag "User" (UTF-16BE, +8), no online
// XUID (+0x28), cached user flags = country 103 << 8 | language 1 << 25 (the console's XConfig policy values, as
// Xenia's profile manager takes them from its configuration; +0x30), everything else 0.
namespace {
constexpr uint32_t kProfileItemSize = 0x188;  // XUID 8 + account 0x17C + device id 4

std::mutex profileEnumLock;
std::map<uint32_t, uint32_t> profileEnumerators;  // object body -> items already returned

void WriteLocalProfile(uint8_t* base, uint32_t at)
{
    using runtime::Store32;
    std::memset(base + at, 0, kProfileItemSize);
    Store32(base, at, uint32_t(runtime::user::kXuid >> 32)), Store32(base, at + 4, uint32_t(runtime::user::kXuid));
    const uint32_t account = at + 8;
    for (uint32_t i = 0; runtime::user::kGamertag[i]; i++)
        base[account + 8 + 2 * i + 1] = uint8_t(runtime::user::kGamertag[i]);
    Store32(base, account + 0x30, 103u << 8 | 1u << 25);
    Store32(base, at + 0x184, 1);  // device id
}
}  // namespace

GUEST_IMPORT(XamProfileCreateEnumerator, "REFERENCE_DERIVED: Xenia 838850fed838 xam_enum.cc:166-192 (one item per call)")
{
    // (device id, handle*): an enumerator of the stored profiles (any device: Xenia ignores the id), one per call; 0.
    // No handle pointer: ERROR_INVALID_PARAMETER (0x57).
    const uint32_t handleOut = ctx.r4.u32;
    if (handleOut == 0)
        return (void)(ctx.r3.u64 = kErrorInvalidParameter);
    const uint32_t body = runtime::ArenaAlloc(base, 16);
    {
        std::lock_guard guard(profileEnumLock);
        profileEnumerators[body] = 0;
    }
    runtime::Store32(base, handleOut, runtime::CreateObject(runtime::ObjectType::Enumerator, body, ""));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamProfileEnumerate, "REFERENCE_DERIVED: Xenia 838850fed838 xam_enum.cc:194-203, xenumerator.cc:227-249")
{
    // (handle, flags, buffer*, overlapped*): the next profile (an X_PROFILEENUMRESULT of 0x188 bytes) into the buffer,
    // 0; ERROR_NO_MORE_FILES (0x12) after the last. An unknown handle, flags, or an XOVERLAPPED stops (7).
    const uint32_t handle = ctx.r3.u32, flags = ctx.r4.u32, buffer = ctx.r5.u32, overlapped = ctx.r6.u32;
    const auto object = runtime::LookupHandle(handle);
    std::lock_guard guard(profileEnumLock);
    const auto it = object ? profileEnumerators.find(object->header) : profileEnumerators.end();
    if (it == profileEnumerators.end() || flags || overlapped || buffer == 0)
        Stop(StopCode::Unimplemented, "XamProfileEnumerate(%08X, flags %X, buffer %08X, overlapped %08X) is not modelled", handle, flags,
             buffer, overlapped);
    if (it->second >= 1)  // one stored profile
        return (void)(ctx.r3.u64 = 0x12);
    WriteLocalProfile(base, buffer);
    it->second++;
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamReadBiometricData, "INFERRED: the local profile has no Kinect biometric data (ERROR_NOT_FOUND); Xenia lists the export only")
{
    // (xuid, ...): reads the Kinect face/body data stored for a profile. No Kinect was ever used with this profile:
    // ERROR_NOT_FOUND (0x490), nothing written. FM4 skips its use on any non-zero result (VERIFIED in our translation).
    ctx.r3.u64 = 0x490;
}
