// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Networking: OFFLINE, a compatibility policy, not a claim about any console: the console has no
// network link. The local stack starts (start-up is local and needs no link), the Ethernet link reports
// disconnected, and online services are unavailable. The runtime makes no external connection: no host socket is
// ever opened (sockets are runtime-local, below); every other networking import stays a strict stop until a run
// reaches it and its offline behaviour is specified. Semantics: Xenia xenia-canary 67f16c4e315f src/xenia/kernel/xam/xam_net.cc.
#include "dispatcher.h"
#include "imports.h"
#include "objects.h"
#include "thread_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

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

GUEST_IMPORT(NetDll_XNetRandom, "REFERENCE_DERIVED: XDK XNetRandom (cryptographically random bytes); Xenia xam_net.cc:236-243 is a stub (0xBB fill), not followed")
{
    // (caller, buffer*, length): `length` random bytes from the host's CSPRNG (arc4random_buf), 0 (as Xenia). Local: no
    // link or socket is involved (offline policy). Xenia's constant 0xBB fill (its stub, "makes replicating things
    // easier") would hand every caller identical "random" values, so the documented meaning is kept instead. Caller other
    // than the title (1) stops (7), as XNetStartup here.
    const uint32_t caller = ctx.r3.u32, buffer = ctx.r4.u32, length = ctx.r5.u32;
    if (caller != kCallerTitle)
        Stop(StopCode::Unimplemented, "NetDll_XNetRandom(caller %u): only the title caller (1) is modelled", caller);
    if (length > 0x10000)
        Stop(StopCode::Unimplemented, "NetDll_XNetRandom: %u bytes is outside the modelled sizes (at most 64 KB)", length);
    arc4random_buf(base + buffer, length);
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

// Sockets: runtime-local UDP sockets with no link (a socket() that fails crashes FM4). A socket is an entry in this runtime's
// own table, never a host socket: creation, options, binding and getsockname succeed locally; nothing is ever sent or
// received. Each behaviour below is the one FM4's callers handle (traced: reference/kernel/offline_sockets.md); anything
// FM4 is not seen to use (TCP, blocking or completion-routine I/O, other options) stops.
namespace {

// WinSock error codes (winsock2.h / winerror.h; Xenia xsocket.h X_WSAError uses the same values).
constexpr uint32_t kWsaOperationAborted = 995, kWsaIoIncomplete = 996, kWsaIoPending = 997, kWsaEFault = 10014,
                   kWsaEInval = 10022, kWsaENotSock = 10038, kWsaEAddrInUse = 10048, kWsaENetUnreach = 10051;
constexpr uint32_t kSocketError = 0xFFFFFFFF;  // SOCKET_ERROR and INVALID_SOCKET (-1)
constexpr uint32_t kSolSocket = 0xFFFF, kSoBroadcast = 0x20, kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002;  // winsock2.h
// XWSAOVERLAPPED (Xenia xam_net.cc XWSAOVERLAPPED): Internal +0, InternalHigh +4, Offset +8, OffsetHigh +12, event
// handle +16. Internal holds the NTSTATUS: STATUS_PENDING while the receive waits, STATUS_CANCELLED once closesocket
// aborts it (NT; WinSock reports the latter as WSA_OPERATION_ABORTED).
constexpr uint32_t kOverlappedEvent = 16, kStatusPending = 0x103, kStatusCancelled = 0xC0000120;
// Buffer sizes until set: UNVERIFIED for the Xbox 360 (no reference states its default); 8192 is WinSock's documented
// default and the size FM4 itself presets before asking (reference/kernel/offline_sockets.md, buffer query).
constexpr uint32_t kDefaultBufferSize = 8192;
// Socket values: this runtime's own (FM4 only passes them back to NetDll imports), outside the kernel handle range.
constexpr uint32_t kFirstSocket = 0x50000004;

struct LocalSocket {
    bool vdp = false;
    bool bound = false;
    uint16_t port = 0;
    uint32_t address = 0;  // big-endian in guest memory; kept as read
    uint32_t sendBuffer = kDefaultBufferSize, receiveBuffer = kDefaultBufferSize, broadcast = 0;
};

std::mutex socketLock;
std::map<uint32_t, LocalSocket> sockets;
std::map<uint32_t, uint32_t> pendingReceives;  // overlapped -> socket
std::set<uint32_t> abortedReceives;            // overlapped aborted by closesocket, not yet collected
uint32_t nextSocket = kFirstSocket;
uint16_t nextEphemeralPort = 49152;  // IANA dynamic range (RFC 6335)

void SetLastError(PPCContext& ctx, uint8_t* base, uint32_t error)
{
    runtime::Store32(base, runtime::CurrentThread(ctx, base) + runtime::kthread::kLastError, error);
}

uint32_t Fail(PPCContext& ctx, uint8_t* base, uint32_t error)
{
    SetLastError(ctx, base, error);
    return kSocketError;
}

void RequireTitle(const PPCContext& ctx, const char* import)
{
    if (ctx.r3.u32 != kCallerTitle)
        Stop(StopCode::Unimplemented, "%s(caller %u): only the title caller (1) is modelled", import, ctx.r3.u32);
}

void Log(const char* import, const char* detail)
{
    static std::mutex logLock;
    static std::map<std::string, uint32_t> counts;
    std::lock_guard guard(logLock);
    if (const uint32_t n = ++counts[import]; n <= 3 || n % 100 == 0)
        std::fprintf(stderr, "[net] %s: %s (local socket, no link; call %u)\n", import, detail, n);
}

// The event of an overlapped receive: 0 (none) or an Event handle (anything else stops); its dispatcher header or 0.
uint32_t OverlappedEvent(const uint8_t* base, uint32_t overlapped, const char* import)
{
    const uint32_t handle = runtime::Load32(base, overlapped + kOverlappedEvent);
    if (handle == 0)
        return 0;
    const auto object = runtime::LookupHandle(handle);
    if (!object || object->type != runtime::ObjectType::Event)
        Stop(StopCode::Unimplemented, "%s: overlapped %08X names %08X, not an event", import, overlapped, handle);
    return object->header;
}

uint32_t SocketOption(uint32_t level, uint32_t name, const char* import)
{
    if (level != kSolSocket || (name != kSoBroadcast && name != kSoSndBuf && name != kSoRcvBuf))
        Stop(StopCode::Unimplemented, "%s(level %#x, option %#x): only SO_BROADCAST, SO_SNDBUF and SO_RCVBUF are modelled",
             import, level, name);
    return name;
}

}  // namespace

GUEST_IMPORT(NetDll_socket, "REFERENCE_DERIVED: Xenia xam_net.cc:646-662 (a socket handle); compatibility policy: offline sockets (runtime-local, no host socket)")
{
    // (caller, address family, type, protocol): AF_INET (2) datagram sockets: SOCK_DGRAM (2) with protocol 0, UDP (17)
    // or VDP (254, Xbox's UDP-based game protocol; Xenia xsocket.h IPPROTO_VDP), as FM4 creates them. A new local
    // socket's value. TCP and every other kind stop (7): the offline-sockets policy covers UDP only.
    RequireTitle(ctx, "NetDll_socket");
    const uint32_t family = ctx.r4.u32, type = ctx.r5.u32, protocol = ctx.r6.u32;
    if (family != 2 || type != 2 || (protocol != 0 && protocol != 17 && protocol != 254))
        Stop(StopCode::Unimplemented, "NetDll_socket(af %u, type %u, protocol %u): only AF_INET datagram sockets (UDP or VDP) are modelled",
             family, type, protocol);
    std::lock_guard guard(socketLock);
    const uint32_t handle = nextSocket;
    nextSocket += 4;
    sockets[handle].vdp = protocol == 254;
    (void)base;
    Log("NetDll_socket", protocol == 254 ? "VDP datagram socket" : "UDP datagram socket");
    ctx.r3.u64 = handle;
}

GUEST_IMPORT(NetDll_closesocket, "REFERENCE_DERIVED: Xenia xam_net.cc:664-678; WinSock closesocket (outstanding overlapped I/O is aborted)")
{
    // (caller, socket): 0; the socket's port is free again. A receive still pending on it completes as aborted:
    // Internal = STATUS_CANCELLED, InternalHigh = 0, its event set (WinSock: WSA_OPERATION_ABORTED). Unknown: WSAENOTSOCK.
    RequireTitle(ctx, "NetDll_closesocket");
    std::lock_guard guard(socketLock);
    const uint32_t handle = ctx.r4.u32;
    if (!sockets.erase(handle))
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    for (auto it = pendingReceives.begin(); it != pendingReceives.end();)
    {
        if (it->second != handle)
        {
            ++it;
            continue;
        }
        const uint32_t overlapped = it->first;
        runtime::Store32(base, overlapped, kStatusCancelled);
        runtime::Store32(base, overlapped + 4, 0);
        abortedReceives.insert(overlapped);
        it = pendingReceives.erase(it);
        if (const uint32_t event = OverlappedEvent(base, overlapped, "NetDll_closesocket"))
            runtime::SetEvent(base, event);
    }
    Log("NetDll_closesocket", "closed");
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_shutdown, "REFERENCE_DERIVED: Xenia xam_net.cc:680-695; WinSock shutdown")
{
    // (caller, socket, how): 0 for SD_RECEIVE, SD_SEND or SD_BOTH (0-2); nothing is in flight to stop. Another `how`
    // is WSAEINVAL, an unknown socket WSAENOTSOCK. FM4 ignores the result (reference/kernel/offline_sockets.md, close).
    RequireTitle(ctx, "NetDll_shutdown");
    std::lock_guard guard(socketLock);
    if (!sockets.count(ctx.r4.u32))
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    if (ctx.r5.u32 > 2)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEInval));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_setsockopt, "REFERENCE_DERIVED: Xenia xam_net.cc:697-710; WinSock setsockopt")
{
    // (caller, socket, level, option, value*, length): SO_BROADCAST, SO_SNDBUF, SO_RCVBUF at SOL_SOCKET, a 4-byte
    // value, kept per socket: 0. A shorter value is WSAEFAULT, an unknown socket WSAENOTSOCK; other options stop (7).
    RequireTitle(ctx, "NetDll_setsockopt");
    const uint32_t option = SocketOption(ctx.r5.u32, ctx.r6.u32, "NetDll_setsockopt"), value = ctx.r7.u32, length = ctx.r8.u32;
    std::lock_guard guard(socketLock);
    const auto it = sockets.find(ctx.r4.u32);
    if (it == sockets.end())
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    if (value == 0 || length < 4)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEFault));
    const uint32_t v = runtime::Load32(base, value);
    (option == kSoBroadcast ? it->second.broadcast : option == kSoSndBuf ? it->second.sendBuffer : it->second.receiveBuffer) = v;
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_getsockopt, "REFERENCE_DERIVED: Xenia xam_net.cc:712-726; WinSock getsockopt")
{
    // (caller, socket, level, option, value*, length*): the kept value (defaults above), 4 bytes, length 4: 0. A
    // length under 4 is WSAEFAULT, an unknown socket WSAENOTSOCK; other options stop (7).
    RequireTitle(ctx, "NetDll_getsockopt");
    const uint32_t option = SocketOption(ctx.r5.u32, ctx.r6.u32, "NetDll_getsockopt"), value = ctx.r7.u32, length = ctx.r8.u32;
    std::lock_guard guard(socketLock);
    const auto it = sockets.find(ctx.r4.u32);
    if (it == sockets.end())
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    if (value == 0 || length == 0 || runtime::Load32(base, length) < 4)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEFault));
    const LocalSocket& s = it->second;
    runtime::Store32(base, value, option == kSoBroadcast ? s.broadcast : option == kSoSndBuf ? s.sendBuffer : s.receiveBuffer);
    runtime::Store32(base, length, 4);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_bind, "REFERENCE_DERIVED: Xenia xam_net.cc:748-767; WinSock bind")
{
    // (caller, socket, sockaddr_in*, length): XSOCKADDR_IN (Xenia xam_net.cc: family +0, port +2, address +4, all
    // big-endian). Records the address and port: 0. Port 0 takes the next free port from 49152. A port another local
    // socket holds is WSAEADDRINUSE (FM4 moves on to its next port: reference/kernel/offline_sockets.md, bind), a second
    // bind WSAEINVAL, a length under 16 WSAEFAULT, an unknown socket WSAENOTSOCK; a family other than AF_INET stops.
    RequireTitle(ctx, "NetDll_bind");
    const uint32_t name = ctx.r5.u32, length = ctx.r6.u32;
    std::lock_guard guard(socketLock);
    const auto it = sockets.find(ctx.r4.u32);
    if (it == sockets.end())
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    if (name == 0 || length < 16)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEFault));
    if (const uint16_t family = runtime::Load16(base, name); family != 2)
        Stop(StopCode::Unimplemented, "NetDll_bind: address family %u (only AF_INET is modelled)", family);
    if (it->second.bound)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEInval));
    auto inUse = [](uint16_t port) {
        for (const auto& [h, s] : sockets)
            if (s.bound && s.port == port)
                return true;
        return false;
    };
    uint16_t port = runtime::Load16(base, name + 2);
    if (port == 0)
    {
        for (uint32_t tries = 0; tries < 16384 && inUse(nextEphemeralPort); tries++)
            nextEphemeralPort = nextEphemeralPort == 65535 ? 49152 : nextEphemeralPort + 1;
        if (inUse(nextEphemeralPort))
            Stop(StopCode::Unimplemented, "NetDll_bind: every dynamic port is held");
        port = nextEphemeralPort;
        nextEphemeralPort = nextEphemeralPort == 65535 ? 49152 : nextEphemeralPort + 1;
    }
    else if (inUse(port))
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEAddrInUse));
    it->second.bound = true, it->second.port = port, it->second.address = runtime::Load32(base, name + 4);
    char detail[48];
    std::snprintf(detail, sizeof detail, "bound to port %u", port);
    Log("NetDll_bind", detail);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_getsockname, "REFERENCE_DERIVED: Xenia xam_net.cc:1047-1067; WinSock getsockname")
{
    // (caller, socket, sockaddr*, length*): the bound XSOCKADDR_IN (16 bytes, zero padding), length 16: 0. Unbound:
    // WSAEINVAL; a length under 16 WSAEFAULT; an unknown socket WSAENOTSOCK. FM4 fails its session set-up without it
    // (reference/kernel/offline_sockets.md, getsockname).
    RequireTitle(ctx, "NetDll_getsockname");
    const uint32_t name = ctx.r5.u32, length = ctx.r6.u32;
    std::lock_guard guard(socketLock);
    const auto it = sockets.find(ctx.r4.u32);
    if (it == sockets.end())
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    if (!it->second.bound)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEInval));
    if (name == 0 || length == 0 || runtime::Load32(base, length) < 16)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEFault));
    std::memset(base + name, 0, 16);
    runtime::Store16(base, name, 2);
    runtime::Store16(base, name + 2, it->second.port);
    runtime::Store32(base, name + 4, it->second.address);
    runtime::Store32(base, length, 16);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_WSARecvFrom, "REFERENCE_DERIVED: WinSock WSARecvFrom (overlapped: SOCKET_ERROR + WSA_IO_PENDING); Xenia xam_net.cc:317-335 is a stub (-1), not followed")
{
    // (caller, socket, buffers*, count, received*, flags*, from*, from length*, overlapped*, completion routine): with
    // no link nothing ever arrives, so an overlapped receive stays pending, as on a console with the cable unplugged:
    // SOCKET_ERROR, last error WSA_IO_PENDING, Internal = STATUS_PENDING, its event reset (NT starts an I/O with the
    // event cleared), nothing else written. FM4 keeps such a receive waiting on its event; any other error but
    // WSAENOTSOCK and WSAECONNRESET (which it retries at once) fails its session (reference/kernel/offline_sockets.md, receive).
    // An unbound socket is WSAEINVAL, an unknown one WSAENOTSOCK. A blocking receive (no overlapped) would never return
    // and a completion routine is not modelled: both stop (7), as does reusing a still-pending overlapped.
    using runtime::Argument;
    RequireTitle(ctx, "NetDll_WSARecvFrom");
    const uint32_t overlapped = Argument(ctx, base, 8), completion = Argument(ctx, base, 9);
    std::lock_guard guard(socketLock);
    const auto it = sockets.find(ctx.r4.u32);
    if (it == sockets.end())
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    if (overlapped == 0 || completion != 0)
        Stop(StopCode::Unimplemented, "NetDll_WSARecvFrom(overlapped %08X, completion %08X): only overlapped receives with an event are modelled",
             overlapped, completion);
    if (!it->second.bound)
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaEInval));
    if (pendingReceives.count(overlapped))
        Stop(StopCode::Unimplemented, "NetDll_WSARecvFrom: overlapped %08X is already pending", overlapped);
    if (const uint32_t event = OverlappedEvent(base, overlapped, "NetDll_WSARecvFrom"))
        runtime::ResetEvent(base, event);
    runtime::Store32(base, overlapped, kStatusPending);
    runtime::Store32(base, overlapped + 4, 0);
    abortedReceives.erase(overlapped);
    pendingReceives[overlapped] = ctx.r4.u32;
    Log("NetDll_WSARecvFrom", "pending (nothing arrives without a link)");
    ctx.r3.u64 = Fail(ctx, base, kWsaIoPending);
}

GUEST_IMPORT(NetDll_WSAGetOverlappedResult, "REFERENCE_DERIVED: WinSock WSAGetOverlappedResult")
{
    // (caller, socket, overlapped*, transferred*, wait, flags*): FALSE (0) for each receive this runtime holds: still
    // pending: WSA_IO_INCOMPLETE; aborted by closesocket: WSA_OPERATION_ABORTED, transferred 0 (once). FM4 passes
    // wait 0 (reference/kernel/offline_sockets.md, completion); waiting would never end and stops (7), as does an overlapped
    // this runtime never saw.
    RequireTitle(ctx, "NetDll_WSAGetOverlappedResult");
    const uint32_t overlapped = ctx.r5.u32, transferred = ctx.r6.u32, wait = ctx.r7.u32;
    std::lock_guard guard(socketLock);
    if (pendingReceives.count(overlapped))
    {
        if (wait != 0)
            Stop(StopCode::Unimplemented, "NetDll_WSAGetOverlappedResult(wait): the receive at %08X never completes offline", overlapped);
        return (void)(SetLastError(ctx, base, kWsaIoIncomplete), ctx.r3.u64 = 0);
    }
    if (!abortedReceives.erase(overlapped))
        Stop(StopCode::Unimplemented, "NetDll_WSAGetOverlappedResult: overlapped %08X is no receive of this runtime", overlapped);
    if (transferred != 0)
        runtime::Store32(base, transferred, 0);
    SetLastError(ctx, base, kWsaOperationAborted);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NetDll_WSASendTo, "REFERENCE_DERIVED: WinSock WSASendTo; offline policy (no link: WSAENETUNREACH); Xenia xam_net.cc:339-378 sends on a host socket, not followed")
{
    // (caller, socket, buffers*, count, sent*, flags, to*, to length, overlapped*, completion routine): nothing leaves
    // the machine: SOCKET_ERROR, last error WSAENETUNREACH, `sent` untouched. FM4 ignores the result
    // (reference/kernel/offline_sockets.md, send). An unknown socket is WSAENOTSOCK; an overlapped send or a completion
    // routine stops (7).
    using runtime::Argument;
    RequireTitle(ctx, "NetDll_WSASendTo");
    const uint32_t overlapped = Argument(ctx, base, 8), completion = Argument(ctx, base, 9);
    std::lock_guard guard(socketLock);
    if (!sockets.count(ctx.r4.u32))
        return (void)(ctx.r3.u64 = Fail(ctx, base, kWsaENotSock));
    if (overlapped != 0 || completion != 0)
        Stop(StopCode::Unimplemented, "NetDll_WSASendTo(overlapped %08X, completion %08X): only plain sends are modelled", overlapped, completion);
    Log("NetDll_WSASendTo", "WSAENETUNREACH");
    ctx.r3.u64 = Fail(ctx, base, kWsaENetUnreach);
}

GUEST_IMPORT(NetDll_WSAGetLastError, "REFERENCE_DERIVED: Xenia xam_net.cc:312-315 (the calling thread's last error)")
{
    // (): the calling thread's last error (KTHREAD +0x160), where the NetDll imports store their WSA errors.
    ctx.r3.u64 = runtime::Load32(base, runtime::CurrentThread(ctx, base) + runtime::kthread::kLastError);
}
