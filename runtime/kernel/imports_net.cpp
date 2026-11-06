// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Networking: OFFLINE, a compatibility policy, not a claim about any console: the console has no
// network link. The local stack starts (start-up is local and needs no link), the Ethernet link reports
// disconnected, and online services are unavailable. The runtime makes no external connection: no host socket is
// ever opened; every other networking import stays a strict stop until a run reaches it and its offline behaviour
// is specified. Semantics: Xenia xenia-canary 67f16c4e315f src/xenia/kernel/xam/xam_net.cc.
#include "imports.h"
#include <cstring>
#include <mutex>

namespace {

// XNetStartupParams (Xenia xam_net.cc:154-168): 13 bytes, the first is the structure's own size.
constexpr uint32_t kStartupParamsSize = 13;
constexpr uint32_t kCallerTitle = 1;  // XNCALLER_TITLE: FM4 passes 1 (OBSERVED, attempt trace5)

std::mutex netLock;
bool started = false;
uint8_t startupParams[kStartupParamsSize] = {};
uint32_t requestedVersion = 0;

void Startup(PPCContext& ctx, const uint8_t* base, const char* import, uint32_t versionRequest)
{
    const uint32_t caller = ctx.r3.u32, params = ctx.r4.u32;
    if (caller != kCallerTitle)
        Stop(StopCode::Unimplemented, "%s(caller %u): only the title caller (1) is modelled", import, caller);
    if (params != 0 && base[params] != kStartupParamsSize)
        Stop(StopCode::Unimplemented, "%s: XNetStartupParams of size %u, expected %u", import, base[params], kStartupParamsSize);
    std::lock_guard guard(netLock);
    if (params != 0)
        std::memcpy(startupParams, base + params, kStartupParamsSize);
    requestedVersion = versionRequest;
    started = true;
    ctx.r3.u64 = 0;  // the local stack is up (Xenia xam_net.cc:172-191 returns 0); no link is needed for start-up
}

}  // namespace

GUEST_IMPORT(NetDll_XNetStartupEx, "REFERENCE_DERIVED: Xenia xam_net.cc:195 NetDll_XNetStartupEx (offline policy)")
{
    // (caller, params*, version request). FM4 passes version 0x20511C00 (2.0.20764); Xenia ignores the argument
    // (MW3/Ghosts pass 0x20501400). Whether it is a minimum the system must meet is UNVERIFIED: recorded, not gated.
    Startup(ctx, base, "NetDll_XNetStartupEx", ctx.r5.u32);
}

GUEST_IMPORT(NetDll_XNetStartup, "REFERENCE_DERIVED: Xenia xam_net.cc:172 NetDll_XNetStartup (offline policy)")
{
    Startup(ctx, base, "NetDll_XNetStartup", 0);
}

GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus, "REFERENCE_DERIVED: Xenia xam_net.cc:557 (0); offline policy: no link")
{
    // (caller): the XNET_ETHERNET_LINK_* flags; 0 = no link (offline policy; Xenia also returns 0).
    (void)base;
    ctx.r3.u64 = 0;
}

namespace runtime {
bool NetworkStarted(uint32_t& versionRequest, uint8_t (&params)[13])
{
    std::lock_guard guard(netLock);
    versionRequest = requestedVersion;
    std::memcpy(params, startupParams, sizeof params);
    return started;
}
}  // namespace runtime

namespace {

// X_WSADATA (Xenia xam_net.cc:90-98, big-endian): version +0, high version +2, description[257] +4, system status[129]
// +0x105, max sockets +0x186, max UDP datagram +0x188, vendor info* (+0x18C on Xenia's layout; left untouched: Xenia
// notes a title checks it is round-tripped).
void WsaStartup(PPCContext& ctx, uint8_t* base, const char* import)
{
    const uint32_t caller = ctx.r3.u32, version = ctx.r4.u32 & 0xFFFF, data = ctx.r5.u32;
    // A version word is major in the low byte, minor in the high byte (MAKEWORD): 0x0202 is 2.2, 0x0002 is 2.0.
    if (caller != kCallerTitle || (version & 0xFF) != 2 || (version >> 8) > 2)
        Stop(StopCode::Unimplemented, "%s(caller %u, version %04X): only the title caller and WinSock 2.0-2.2 are modelled", import, caller, version);
    if (data != 0)
    {
        // WinSock WSAStartup semantics: wVersion is the requested version (supported here: 2.0-2.2; Xenia off Windows
        // also returns the request), wHighVersion the highest supported, 2.2; description and status empty;
        // iMaxSockets and iMaxUdpDg are 0: WinSock 2 documents them as ignored (Xenia off Windows guesses 100 and 1024).
        // The WORDs are big-endian in guest memory.
        base[data] = uint8_t(version >> 8), base[data + 1] = uint8_t(version), base[data + 2] = 0x02, base[data + 3] = 0x02;
        base[data + 4] = 0, base[data + 0x105] = 0;
        base[data + 0x186] = 0, base[data + 0x187] = 0, base[data + 0x188] = 0, base[data + 0x189] = 0;
    }
    ctx.r3.u64 = 0;  // the local sockets layer starts; no link and no host socket are involved
}

}  // namespace

GUEST_IMPORT(NetDll_WSAStartupEx, "REFERENCE_DERIVED: Xenia xam_net.cc:296 (forwards to WSAStartup); offline policy")
{
    // (caller, version, WSADATA*, version request): the version request is recorded nowhere and not gated (as Xenia).
    WsaStartup(ctx, base, "NetDll_WSAStartupEx");
}

GUEST_IMPORT(NetDll_WSAStartup, "REFERENCE_DERIVED: Xenia xam_net.cc:246 NetDll_WSAStartup; offline policy")
{
    WsaStartup(ctx, base, "NetDll_WSAStartup");
}
