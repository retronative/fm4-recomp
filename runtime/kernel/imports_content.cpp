// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Content and enumeration imports over the saved-content storage (content.h). Semantics: Xenia xenia-canary
// 67f16c4e315f kernel/xam/xam_content.cc, xam_enum.cc, kernel/xenumerator.{h,cc}.
#include "imports.h"
#include "arena.h"
#include "content.h"
#include "objects.h"
#include "file_system.h"
#include "overlapped.h"
#include "stfs.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t kErrorInvalidHandle = 0x6, kErrorNoMoreFiles = 0x12, kErrorInvalidParameter = 0x57, kInvalidArg = (1u << 31) | (7u << 16) | 0x57;  // E_INVALIDARG: severity, FACILITY_WIN32 7, ERROR_INVALID_PARAMETER (xbox.h:113-122)

struct Enumerator { std::vector<runtime::ContentData> items; uint32_t perCall; size_t next; };
std::mutex enumLock;
std::map<uint32_t, Enumerator> enumerators;  // by object body

}  // namespace

GUEST_IMPORT(XamContentCreateEnumerator, "REFERENCE_DERIVED: Xenia xam_content.cc:72 XamContentCreateEnumerator, xenumerator.cc")
{
    // (user index, device id, content type, flags, items per call, buffer size*, handle*). FM4: (0xFE, 0 = any device,
    // 1 = saved game, 0, 5, ...). A device other than 0 or the content device: E_INVALIDARG with buffer size 0 (Xenia).
    // Only saved games and flags 0 are modelled; anything else stops (7). The enumerator holds what is stored now.
    using namespace runtime;
    const uint32_t userIndex = ctx.r3.u32, device = ctx.r4.u32, type = ctx.r5.u32, flags = ctx.r6.u32, perCall = ctx.r7.u32;
    const uint32_t sizeOut = ctx.r8.u32, handleOut = ctx.r9.u32;
    if (device != 0 && device != kContentDeviceId)
    {
        if (sizeOut)
            Store32(base, sizeOut, 0);
        return (void)(ctx.r3.u64 = kInvalidArg);
    }
    if (type != kContentTypeSavedGame || flags != 0 || perCall == 0 || handleOut == 0)
        Stop(StopCode::Unimplemented, "XamContentCreateEnumerator(user %X, device %u, type %X, flags %X, %u per call): not modelled",
             userIndex, device, type, flags, perCall);
    if (sizeOut)
        Store32(base, sizeOut, kContentDataSize * perCall);
    // X_KENUMERATOR (xenumerator.h:23): app id, open message, close message, user index, items per call, flags; Xenia
    // Initialize(0xFF, 0xFE, 0x20005, 0x20007, 0) gives user index 0xFF and app id 0xFE.
    const uint32_t body = ArenaAlloc(base, 0x18);
    const uint32_t words[6] = {0xFE, 0x20005, 0x20007, 0xFF, perCall, 0};
    for (int i = 0; i < 6; i++)
        Store32(base, body + 4 * i, words[i]);
    {
        std::lock_guard guard(enumLock);
        enumerators[body] = Enumerator{ListContent(type), perCall, 0};
    }
    Store32(base, handleOut, CreateObject(ObjectType::Enumerator, body, ""));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamEnumerate, "REFERENCE_DERIVED: Xenia xam_enum.cc:30-86 xeXamEnumerate, xenumerator.cc:60 WriteItems")
{
    // (handle, flags, buffer*, buffer length, items returned*, overlapped*). Synchronous (items returned given): up to the
    // enumerator's items per call, then ERROR_NO_MORE_FILES (0x12) with 0 items. A buffer too small for them stops (7)
    // (Xenia does not check); flags other than 0 or the overlapped form stop (7).
    using namespace runtime;
    const uint32_t handle = ctx.r3.u32, flags = ctx.r4.u32, buffer = ctx.r5.u32, length = ctx.r6.u32, returned = ctx.r7.u32;
    const uint32_t overlapped = ctx.r8.u32;
    if (flags != 0 || overlapped != 0 || returned == 0)
        Stop(StopCode::Unimplemented, "XamEnumerate(%08X, flags %X, overlapped %08X): only the synchronous form is modelled", handle, flags, overlapped);
    const auto object = LookupHandle(handle);
    std::lock_guard guard(enumLock);
    const auto it = object && object->type == ObjectType::Enumerator ? enumerators.find(object->header) : enumerators.end();
    if (it == enumerators.end())
        return (void)(ctx.r3.u64 = kErrorInvalidHandle);
    if (buffer == 0)
    {
        Store32(base, returned, 0);
        return (void)(ctx.r3.u64 = kErrorInvalidParameter);
    }
    Enumerator& e = it->second;
    const size_t count = std::min<size_t>(e.items.size() - e.next, e.perCall);
    if (count == 0)
    {
        Store32(base, returned, 0);
        return (void)(ctx.r3.u64 = kErrorNoMoreFiles);
    }
    if (uint64_t(count) * kContentDataSize > length)
        Stop(StopCode::Unimplemented, "XamEnumerate: %zu items need %llu bytes, the buffer has %u", count,
             (unsigned long long)(count * kContentDataSize), length);
    for (size_t i = 0; i < count; i++)
        std::memcpy(base + buffer + i * kContentDataSize, e.items[e.next + i].data(), kContentDataSize);
    e.next += count;
    Store32(base, returned, uint32_t(count));
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamContentOpenFile, "REFERENCE_DERIVED: XContent package mounting as Xenia content roots (xam_content.cc); Xenia stubs this call (:267-276, FILE_NOT_FOUND)")
{
    // (user index, root name*, path*, flags, disposition*, license mask*, overlapped*): opens a content package file
    // as the root "<root name>:". An update:\ path: no title update is installed (owner policy, as update:\media.zip),
    // ERROR_FILE_NOT_FOUND (2). A package on the disc (FM4: game:\Database.xmplr, a PIRS STFS package): mounted
    // read-only (kernel/stfs.h, file_system.h MountContentRoot), 0. Xenia answers FILE_NOT_FOUND for every path (its
    // own "TODO": arguments assumed), which here would deny a file that exists. A missing file: ERROR_FILE_NOT_FOUND.
    // Not modelled, so they stop (7): disposition or license outputs, an overlapped call, a root already mounted, a
    // path on another device, a directory.
    const uint32_t root = ctx.r4.u32, path = ctx.r5.u32, disposition = ctx.r7.u32, license = ctx.r8.u32;
    const uint32_t overlapped = runtime::Argument(ctx, base, 6);
    const char* rootName = reinterpret_cast<const char*>(base + root);
    const char* text = reinterpret_cast<const char*>(base + path);
    if (disposition || license || overlapped)
        Stop(StopCode::Unimplemented, "XamContentOpenFile(\"%.32s\", \"%.64s\") with disposition %08X, license %08X, overlapped %08X "
             "is not modelled", rootName, text, disposition, license, overlapped);
    if (std::strncmp(text, "update:\\", 8) == 0)
        return (void)(ctx.r3.u64 = 2);  // ERROR_FILE_NOT_FOUND
    runtime::DiscEntry entry;
    std::string device;
    const runtime::Resolved r = runtime::ResolveGuestPath(text, entry, device);
    if (r == runtime::Resolved::OtherDevice || entry.package || (r == runtime::Resolved::Found && entry.directory))
        Stop(StopCode::Unimplemented, "XamContentOpenFile(\"%.32s\", \"%.64s\"): only a package file on the disc is modelled", rootName, text);
    if (r == runtime::Resolved::NotFound)
        return (void)(ctx.r3.u64 = 2);
    const auto package = runtime::stfs::Package::Open(entry.host);
    if (!runtime::MountContentRoot(rootName, package))
        Stop(StopCode::Unimplemented, "XamContentOpenFile: root \"%.32s\" is already mounted", rootName);
    std::fprintf(stderr, "[content] %s: mounted %s (%zu entries) read-only\n", rootName, text, package->Files().size());
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamContentClose, "REFERENCE_DERIVED: Xenia xam_content.cc:290-303 (CloseContent; overlapped completed at once)")
{
    // (root name*, overlapped*): unmounts a root XamContentOpenFile mounted, 0 (with an XOVERLAPPED: completed with 0,
    // ERROR_IO_PENDING). A root that is not mounted stops (7) (no other way to mount roots exists here yet).
    const uint32_t root = ctx.r3.u32, overlapped = ctx.r4.u32;
    const char* rootName = reinterpret_cast<const char*>(base + root);
    if (overlapped)
        runtime::CheckOverlapped(ctx, base, overlapped, "XamContentClose");
    if (!runtime::UnmountContentRoot(rootName))
        Stop(StopCode::Unimplemented, "XamContentClose(\"%.32s\"): no such root is mounted", rootName);
    ctx.r3.u64 = runtime::FinishOverlapped(ctx, base, overlapped, 0, 0, 0, "XamContentClose");
}

GUEST_IMPORT(XamCacheOpenFile, "INFERRED: the XAM cache lives on the hard disk, which is absent (owner policy): ERROR_DEVICE_NOT_CONNECTED; Xenia lists the export only")
{
    // (request*, ...): opens a file of the XAM cache (the hard disk's cache partition). No hard disk is attached (owner
    // policy, as \Device\Harddisk0): ERROR_DEVICE_NOT_CONNECTED (0x48F), nothing written. FM4 treats any non-zero result
    // as "no cache file" and clears its key (VERIFIED in our translation).
    ctx.r3.u64 = 0x48F;
}

GUEST_IMPORT(XamCacheReset, "INFERRED: no hard disk, no XAM cache to reset (ERROR_DEVICE_NOT_CONNECTED); Xenia lists the export only")
{
    // (cache id, ...): clears a XAM cache. None exists without a hard disk (owner policy): ERROR_DEVICE_NOT_CONNECTED
    // (0x48F). FM4 discards the result (VERIFIED in our translation).
    ctx.r3.u64 = 0x48F;
}
