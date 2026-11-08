// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Content and enumeration imports over the saved-content storage (content.h). Semantics: Xenia xenia-canary
// 67f16c4e315f kernel/xam/xam_content.cc, xam_enum.cc, kernel/xenumerator.{h,cc}.
#include "imports.h"
#include "module.h"
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

// The items an enumerator returns, each `itemSize` bytes as XamEnumerate copies them (content records, achievement details).
struct Enumerator { std::vector<std::vector<uint8_t>> items; uint32_t itemSize; uint32_t perCall; size_t next; };
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
    // Saved games and installed downloadable content are listed from what the user-data store actually holds (empty
    // at first); nothing installs marketplace content here, so its list is the
    // honest empty one (FM4 enumerates it after the device selector). Other types stop.
    if ((type != kContentTypeSavedGame && type != kContentTypeMarketplace) || flags != 0 || perCall == 0 || handleOut == 0)
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
        std::vector<std::vector<uint8_t>> items;
        for (const ContentData& item : ListContent(type))
            items.emplace_back(item.begin(), item.end());
        enumerators[body] = Enumerator{std::move(items), kContentDataSize, perCall, 0};
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
    if (uint64_t(count) * e.itemSize > length)
        Stop(StopCode::Unimplemented, "XamEnumerate: %zu items need %llu bytes, the buffer has %u", count,
             (unsigned long long)(count * e.itemSize), length);
    for (size_t i = 0; i < count; i++)
        std::memcpy(base + buffer + i * e.itemSize, e.items[e.next + i].data(), e.itemSize);
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

GUEST_IMPORT(XamContentCreateEx, "REFERENCE_DERIVED: Xenia xam_content.cc:124-243 xeXamContentCreate, content_manager.cc:215-239 CreateContent; compatibility policy: save storage")
{
    // (user, root name, XCONTENT_DATA*, flags, disposition*, license mask*, cache size, content size (r10), XOVERLAPPED*
    // (stack)). Modelled: OPEN_EXISTING (flags & 0xF == 3): PATH_NOT_FOUND (3) if the saved game is not stored, else
    // OpenContent (content.h: the stored directory mounted as the writable root "<root>:"; disposition state 2, open); and
    // CREATE_NEW (1): ALREADY_EXISTS (0xB7) if stored, else CreateContent (content.h: the directory, its record, the
    // writable root "<root>:"). Xenia's results: the Win32 result; with an XOVERLAPPED, IO_PENDING and the completion
    // {result, or FUNCTION_FAILED 0x65B on failure; extended error HRESULT_FROM_WIN32(result); length = the disposition
    // state (0 unknown, 1 created)}; the disposition pointer zeroed first when both are given (:143-145), the license mask
    // 0 on success (:207-209). Other dispositions, another device or type stop (7).
    const uint32_t user = ctx.r3.u32, rootName = ctx.r4.u32, dataPtr = ctx.r5.u32, flags = ctx.r6.u32, dispositionPtr = ctx.r7.u32,
                   licensePtr = ctx.r8.u32, overlapped = runtime::Argument(ctx, base, 8);
    runtime::ContentData data;
    std::memcpy(data.data(), base + dataPtr, data.size());
    const uint32_t device = runtime::Load32(base, dataPtr), type = runtime::Load32(base, dataPtr + 4), disposition = flags & 0xF;
    const std::string root(reinterpret_cast<const char*>(base + rootName), strnlen(reinterpret_cast<const char*>(base + rootName), 64));
    if ((device != runtime::kContentDeviceId && device != 0) || type != runtime::kContentTypeSavedGame)
        Stop(StopCode::Unimplemented, "XamContentCreateEx(\"%s\", device %u, type %08X): only saved games on device 1 are modelled", root.c_str(), device, type);
    const std::string fileName = runtime::ContentFileName(data);
    if (disposition != 1 && disposition != 3)
        Stop(StopCode::Unimplemented, "XamContentCreateEx(\"%s\", \"%s\", flags %08X): disposition %u is not modelled yet", root.c_str(), fileName.c_str(),
             flags, disposition);
    const bool stored = runtime::ContentExists(data);
    if (overlapped != 0)
        runtime::CheckOverlapped(ctx, base, overlapped, "XamContentCreateEx");
    constexpr uint32_t kPathNotFound = 3, kAlreadyExists = 0xB7, kFunctionFailed = 0x65B;
    uint32_t result = 0, state = 0;  // Xenia kDispositionState: 0 unknown, 1 create, 2 open
    if (disposition == 3 && !stored)
        result = kPathNotFound;
    else if (disposition == 3)
        state = 2, result = runtime::OpenContent(data, root);
    else if (stored)
        result = kAlreadyExists;
    else
        state = 1, result = runtime::CreateContent(data, root);
    std::fprintf(stderr, "[content] XamContentCreateEx(user %u, \"%s\", saved game \"%s\", %s): %s (result %X)\n", user, root.c_str(), fileName.c_str(),
                 disposition == 3 ? "OPEN_EXISTING" : "CREATE_NEW", result != 0 ? "not done" : state == 2 ? "opened and mounted" : "created and mounted", result);
    if (licensePtr != 0 && result == 0)
        runtime::Store32(base, licensePtr, 0);
    if (overlapped == 0)
        return (void)(ctx.r3.u64 = result);
    if (dispositionPtr != 0)
        runtime::Store32(base, dispositionPtr, 0);
    const uint32_t extended = result == 0 ? 0 : (1u << 31 | 0x70000 | (result & 0xFFFF));  // HRESULT_FROM_WIN32
    ctx.r3.u64 = runtime::FinishOverlapped(ctx, base, overlapped, result == 0 ? 0 : kFunctionFailed, extended, state, "XamContentCreateEx");
}

GUEST_IMPORT(XamContentGetDeviceData, "REFERENCE_DERIVED: Xenia xam_content_device.cc:101-129 (X_CONTENT_DEVICE_DATA, 0x50 bytes); compatibility policy: save storage (512 MB quota)")
{
    // (device id, X_CONTENT_DEVICE_DATA*): the only device (1): zeroed, then id, type HDD (1), total bytes = the quota,
    // free bytes = the quota minus what is stored (content.h; not Xenia's invented 20 GB / 3 GB), and Xenia's name
    // "Dummy HDD" (UTF-16 big-endian, at most 28 characters with the terminator). Any other id: DEVICE_NOT_CONNECTED
    // (0x48F), nothing written.
    const uint32_t deviceId = ctx.r3.u32, data = ctx.r4.u32;
    if (deviceId != runtime::kContentDeviceId)
        return (void)(ctx.r3.u64 = 0x48F);
    const uint64_t used = runtime::ContentBytesUsed(), total = runtime::kContentQuota, free = used >= total ? 0 : total - used;
    std::memset(base + data, 0, 0x50);
    runtime::Store32(base, data, runtime::kContentDeviceId);
    runtime::Store32(base, data + 4, runtime::kContentDeviceType);
    runtime::Store32(base, data + 8, uint32_t(total >> 32)), runtime::Store32(base, data + 12, uint32_t(total));
    runtime::Store32(base, data + 16, uint32_t(free >> 32)), runtime::Store32(base, data + 20, uint32_t(free));
    constexpr char kName[] = "Dummy HDD";
    for (size_t i = 0; i < sizeof kName - 1; i++)
        base[data + 24 + 2 * i + 1] = uint8_t(kName[i]);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamContentGetDeviceState, "REFERENCE_DERIVED: Xenia xam_content_device.cc:78-99 (device present: success; else DEVICE_NOT_CONNECTED; overlapped completed at once)")
{
    // (device id, XOVERLAPPED*): the only device (1) is present: 0. Any other id: DEVICE_NOT_CONNECTED (0x48F), or with
    // an XOVERLAPPED, FUNCTION_FAILED (0x65B) with extended error 0x48F (Xenia's CompleteOverlappedImmediateEx).
    constexpr uint32_t kDeviceNotConnected = 0x48F, kFunctionFailed = 0x65B;
    const uint32_t deviceId = ctx.r3.u32, overlapped = ctx.r4.u32;
    const bool present = deviceId == runtime::kContentDeviceId;
    if (!overlapped)
        return (void)(ctx.r3.u64 = present ? 0 : kDeviceNotConnected);
    ctx.r3.u64 = runtime::FinishOverlapped(ctx, base, overlapped, present ? 0 : kFunctionFailed, present ? 0 : kDeviceNotConnected, 0,
                                           "XamContentGetDeviceState");
}

GUEST_IMPORT(XamContentFlush, "REFERENCE_DERIVED: Xenia xam_content.cc:278-288 (success; overlapped completed at once); the root's host files fsynced (file_system.h)")
{
    // (root name*, overlapped*): 0 (with an XOVERLAPPED: completed with 0, ERROR_IO_PENDING). Xenia does nothing; here a
    // writable root's files are made durable, so a saved game survives the process ending. A root that is not mounted
    // stops (7).
    const uint32_t root = ctx.r3.u32, overlapped = ctx.r4.u32;
    const char* rootName = reinterpret_cast<const char*>(base + root);
    if (overlapped)
        runtime::CheckOverlapped(ctx, base, overlapped, "XamContentFlush");
    if (!runtime::FlushContentRoot(rootName))
        Stop(StopCode::Unimplemented, "XamContentFlush(\"%.32s\"): no such root is mounted", rootName);
    std::fprintf(stderr, "[content] XamContentFlush(\"%.32s\"): flushed\n", rootName);
    ctx.r3.u64 = runtime::FinishOverlapped(ctx, base, overlapped, 0, 0, 0, "XamContentFlush");
}

GUEST_IMPORT(XamContentDelete, "REFERENCE_DERIVED: Xenia xam_content.cc:409-423, content_manager.cc:318-332 DeleteContent (overlapped completed at once)")
{
    // (user index, XCONTENT_DATA*, XOVERLAPPED*): DeleteContent (content.h): 0, ACCESS_DENIED (5) while open, FILE_NOT_FOUND
    // (2) if not stored; with an XOVERLAPPED, completed as Xenia's CompleteOverlappedImmediate (extended error = result) and
    // ERROR_IO_PENDING. Saved games on device 0 or 1 only; another type or device stops (7).
    const uint32_t user = ctx.r3.u32, dataPtr = ctx.r4.u32, overlapped = ctx.r5.u32;
    runtime::ContentData data;
    std::memcpy(data.data(), base + dataPtr, data.size());
    const uint32_t device = runtime::Load32(base, dataPtr), type = runtime::Load32(base, dataPtr + 4);
    if ((device != runtime::kContentDeviceId && device != 0) || type != runtime::kContentTypeSavedGame)
        Stop(StopCode::Unimplemented, "XamContentDelete(device %u, type %08X): only saved games on device 1 are modelled", device, type);
    if (overlapped)
        runtime::CheckOverlapped(ctx, base, overlapped, "XamContentDelete");
    const std::string name = runtime::ContentFileName(data);
    const uint32_t result = runtime::DeleteContent(data);
    std::fprintf(stderr, "[content] XamContentDelete(user %u, saved game \"%s\"): %s (result %X)\n", user, name.c_str(),
                 result == 0 ? "deleted" : "not deleted", result);
    if (!overlapped)
        return (void)(ctx.r3.u64 = result);
    runtime::CompleteOverlappedImmediate(ctx, base, overlapped, result, "XamContentDelete");
    ctx.r3.u64 = 0x3E5;  // ERROR_IO_PENDING
}

GUEST_IMPORT(XamContentClose, "REFERENCE_DERIVED: Xenia xam_content.cc:290-303 (CloseContent; overlapped completed at once)")
{
    // (root name*, overlapped*): unmounts a root XamContentOpenFile or XamContentCreateEx mounted, 0 (with an XOVERLAPPED: completed with 0,
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

GUEST_IMPORT(XamShowDeviceSelectorUI, "REFERENCE_DERIVED: Xenia xam_ui.cc:525-537 (headless: device 1); compatibility policy: save storage (one device, id 1)")
{
    // (user index, content type, content flags, total requested (qword), DWORD* device id, XOVERLAPPED*): the storage-device
    // chooser. This runtime has exactly one content device (content.h kContentDeviceId, the save-storage policy:
    // one device after Xenia, id 1, HDD type), so the answer is that device, as Xenia's headless dispatch writes it, with
    // success; asynchronously through the XOVERLAPPED when one is given (Xenia xeXamDispatchHeadless -> completion,
    // ERROR_IO_PENDING). The answer is logged (every UI answer is logged).
    const uint32_t user = ctx.r3.u32, contentType = ctx.r4.u32, flags = ctx.r5.u32, deviceIdPtr = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (deviceIdPtr == 0)
        Stop(StopCode::Unimplemented, "XamShowDeviceSelectorUI(user %u, content type %08X) without a device id pointer is not modelled", user, contentType);
    runtime::Store32(base, deviceIdPtr, runtime::kContentDeviceId);
    std::fprintf(stderr, "[ui] XamShowDeviceSelectorUI(user %u, content type %08X, flags %08X): answered device %u (the only storage device)\n", user,
                 contentType, flags, runtime::kContentDeviceId);
    ctx.r3.u64 = overlapped != 0 ? runtime::FinishOverlapped(ctx, base, overlapped, 0, 0, 0, "XamShowDeviceSelectorUI") : 0;
}

namespace {

// The title's achievement definitions: the XACH table of its XDBF, the executable's resource named by its title id in
// hex (Xenia kernel_state.cc:119-135 title_xdbf; xdbf_utils.{h,cc}: header 24 bytes {magic "XDBF", version, entry count,
// entries used, free count, free used}, 18-byte entries {section u16, id u64, offset, size}, 8-byte free entries, then
// the content; the metadata section (1) entry "XACH" (0x58414348): {magic, version 1, size, count u16}, then 0x24-byte
// entries {id u16, label, description, unachieved string ids u16, image id u32, gamerscore u16, u16, flags u32, ...}).
// FM4: 48 achievements, 1000 gamerscore (the local capture achievements (not distributed)). A malformed table stops (9).
// The executable's title id (execution info 0x00040006 +0xC; Xenia xex2_info.h:486-490).
uint32_t CurrentTitleId(const uint8_t* base)
{
    const uint32_t execution = runtime::XexHeaderField(base, runtime::ExecutableModule().header, 0x00040006);
    if (!execution)
        Stop(StopCode::Setup, "the executable has no execution info (title id)");
    return runtime::Load32(base, execution + 0xC);
}

struct AchievementDefinition { uint32_t id, imageId, gamerscore, flags; };

std::vector<AchievementDefinition> TitleAchievements(const uint8_t* base)
{
    using runtime::Load32;
    const uint32_t header = runtime::ExecutableModule().header, info = runtime::XexHeaderField(base, header, 0x000002FF);
    if (!info)
        Stop(StopCode::Setup, "achievements: the executable has no resource info");
    char name[9];
    std::snprintf(name, sizeof name, "%08X", CurrentTitleId(base));
    uint32_t data = 0, size = 0;
    for (uint32_t i = 0, count = (Load32(base, info) - 4) / 16; i < count; i++)
        if (std::memcmp(base + info + 4 + 16 * i, name, 8) == 0)
            data = Load32(base, info + 4 + 16 * i + 8), size = Load32(base, info + 4 + 16 * i + 12);
    auto u16 = [&](uint32_t at) { return uint32_t(base[at] << 8 | base[at + 1]); };
    if (!data || size < 24 || Load32(base, data) != 0x58444246)  // "XDBF"
        Stop(StopCode::Setup, "achievements: no XDBF resource %s", name);
    const uint32_t entryCount = Load32(base, data + 8), entriesUsed = Load32(base, data + 12), freeCount = Load32(base, data + 16);
    const uint64_t content = 24 + 18ull * entryCount + 8ull * freeCount;
    if (entriesUsed > entryCount || content > size)
        Stop(StopCode::Setup, "achievements: XDBF %s has a malformed header", name);
    std::vector<AchievementDefinition> out;
    for (uint32_t i = 0; i < entriesUsed; i++)
    {
        const uint32_t e = data + 24 + 18 * i;
        if (u16(e) != 1 || Load32(base, e + 2) != 0 || Load32(base, e + 6) != 0x58414348)
            continue;
        const uint64_t table = content + Load32(base, e + 10), tableSize = Load32(base, e + 14);
        if (table + tableSize > size || tableSize < 14 || Load32(base, data + uint32_t(table)) != 0x58414348 ||
            Load32(base, data + uint32_t(table) + 4) != 1)
            Stop(StopCode::Setup, "achievements: XDBF %s has a malformed XACH table", name);
        const uint32_t at = data + uint32_t(table), count = u16(at + 12);
        if (14 + 0x24ull * count > tableSize)
            Stop(StopCode::Setup, "achievements: XACH of %s lists %u entries past its size", name, count);
        for (uint32_t k = 0; k < count; k++)
        {
            const uint32_t a = at + 14 + 0x24 * k;
            out.push_back({u16(a), Load32(base, a + 8), u16(a + 12), Load32(base, a + 16)});
        }
    }
    return out;
}

}  // namespace

GUEST_IMPORT(XamUserCreateAchievementEnumerator, "REFERENCE_DERIVED: Xenia xam_user.cc:577-749 (XStaticAchievementEnumerator; X_ACHIEVEMENT_DETAILS 36 bytes; the title XDBF's XACH table)")
{
    // (title id, user index, xuid, flags, offset, count, buffer size*, handle*). Xenia: count 0 or a null pointer, or a user
    // index of 4 or more: INVALID_PARAMETER (0x57); buffer size = count * 36 (+ 464 with strings); the enumerator lists the
    // title's achievement definitions as X_ACHIEVEMENT_DETAILS {id, label*, description*, unachieved*, image id,
    // gamerscore, unlock time {0, 0}, flags} (none unlocked: the local offline profile has earned none), `count` per
    // XamEnumerate. X_KENUMERATOR as Xenia's Initialize(user, 0xFB, 0xB000A, 0xB000B, 0). Modelled: the current title
    // (0) and the user's own achievements (xuid 0), offset 0, no strings (flags & 7 == 0); anything else stops (7).
    using namespace runtime;
    const uint32_t titleId = ctx.r3.u32, user = ctx.r4.u32, xuid = ctx.r5.u32, flags = ctx.r6.u32, offset = ctx.r7.u32,
                   count = ctx.r8.u32, sizeOut = ctx.r9.u32, handleOut = ctx.r10.u32;
    if (!count || !sizeOut || !handleOut || user >= 4)
        return (void)(ctx.r3.u64 = kErrorInvalidParameter);
    if (titleId != 0 || xuid != 0 || offset != 0 || (flags & 7) != 0)
        Stop(StopCode::Unimplemented, "XamUserCreateAchievementEnumerator(title %08X, user %u, xuid %08X, flags %X, offset %u): not modelled",
             titleId, user, xuid, flags, offset);
    constexpr uint32_t kDetailsSize = 36;
    Store32(base, sizeOut, kDetailsSize * count);
    std::vector<std::vector<uint8_t>> items;
    for (const auto& a : TitleAchievements(base))
    {
        std::vector<uint8_t> item(kDetailsSize, 0);
        const uint32_t words[9] = {a.id, 0, 0, 0, a.imageId, a.gamerscore, 0, 0, a.flags};
        for (int i = 0; i < 9; i++)
            item[4 * i] = uint8_t(words[i] >> 24), item[4 * i + 1] = uint8_t(words[i] >> 16), item[4 * i + 2] = uint8_t(words[i] >> 8),
            item[4 * i + 3] = uint8_t(words[i]);
        items.push_back(std::move(item));
    }
    const uint32_t body = ArenaAlloc(base, 0x18);
    const uint32_t header[6] = {0xFB, 0xB000A, 0xB000B, user, count, 0};
    for (int i = 0; i < 6; i++)
        Store32(base, body + 4 * i, header[i]);
    const size_t listed = items.size();
    {
        std::lock_guard guard(enumLock);
        enumerators[body] = Enumerator{std::move(items), kDetailsSize, count, 0};
    }
    Store32(base, handleOut, CreateObject(ObjectType::Enumerator, body, ""));
    std::fprintf(stderr, "[achievements] enumerator: %zu achievements of the title, %u per call, none unlocked\n", listed, count);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamContentAggregateCreateEnumerator, "REFERENCE_DERIVED: Xenia xam_content_aggregate.cc:84-145 (XStaticEnumerator<XCONTENT_AGGREGATE_DATA>, 1 per call)")
{
    // (xuid (64 bits), device id, content type, unknown, handle*): FM4 reaches it through XexGetProcedureAddress(xam, 0x279)
    // from its content-enumerator wrapper (items per call 1, buffer 312 = XCONTENT_AGGREGATE_DATA: XCONTENT_DATA + title id
    // at +0x134). Xenia: a null handle pointer or a device that is neither 0 (any) nor known: E_INVALIDARG; on the hard disk
    // (device 0 or 1) the stored items of the current title and of its alternate title ids (FM4: 4D53084D, Forza
    // Motorsport 3); on the disc (device 0) game:\Content\0000000000000000. Here: the user-data store holds only content
    // made by this runtime for the current title, so its items carry the current title id and the alternate title has
    // none; disc 1 has no Content directory, and one that exists stops (not modelled). X_KENUMERATOR as Xenia's
    // Initialize(0xFF, 0xFE, 0x2000E, 0x20010, 0), then {"XEN\0", handle}. Saved games (1) and marketplace content (2)
    // are modelled; another type stops (7).
    using namespace runtime;
    const uint32_t device = ctx.r4.u32, type = ctx.r5.u32, handleOut = ctx.r7.u32;
    if (!handleOut || (device != 0 && device != kContentDeviceId))
        return (void)(ctx.r3.u64 = kInvalidArg);
    if (type != kContentTypeSavedGame && type != kContentTypeMarketplace)
        Stop(StopCode::Unimplemented, "XamContentAggregateCreateEnumerator(device %u, type %X): not modelled", device, type);
    if (device == 0)
    {
        DiscEntry odd;
        std::string resolved;
        if (ResolveGuestPath("game:\\Content\\0000000000000000", odd, resolved) == Resolved::Found)
            Stop(StopCode::Unimplemented, "XamContentAggregateCreateEnumerator: disc content (game:\\Content) is not modelled");
    }
    constexpr uint32_t kAggregateSize = kContentDataSize + 4;
    const uint32_t titleId = CurrentTitleId(base);
    std::vector<std::vector<uint8_t>> items;
    for (const ContentData& item : ListContent(type))
    {
        std::vector<uint8_t> record(item.begin(), item.end());
        for (int i = 0; i < 4; i++)
            record.push_back(uint8_t(titleId >> (24 - 8 * i)));
        items.push_back(std::move(record));
    }
    const uint32_t body = ArenaAlloc(base, 0x20);
    const uint32_t words[6] = {0xFE, 0x2000E, 0x20010, 0xFF, 1, 0};
    for (int i = 0; i < 6; i++)
        Store32(base, body + 4 * i, words[i]);
    const size_t listed = items.size();
    {
        std::lock_guard guard(enumLock);
        enumerators[body] = Enumerator{std::move(items), kAggregateSize, 1, 0};
    }
    const uint32_t handle = CreateObject(ObjectType::Enumerator, body, "");
    Store32(base, body + 0x18, 0x58454E00), Store32(base, body + 0x1C, handle);  // X_KENUMERATOR_CONTENT_AGGREGATE
    Store32(base, handleOut, handle);
    std::fprintf(stderr, "[content] XamContentAggregateCreateEnumerator(device %u, type %X): %zu items\n", device, type, listed);
    ctx.r3.u64 = 0;
}

uint32_t runtime::EnumerateContentAggregateMessage(uint8_t* base, uint32_t data)
{
    // data: {user index, +4, extra* (X_KENUMERATOR_CONTENT_AGGREGATE {"XEN\0", handle}), buffer*, buffer size, +0x14,
    // length*, +0x1C}. An unknown enumerator, a null buffer or extra: E_INVALIDARG (80070057); the buffer is zeroed (its
    // size), then the enumerator's next item written; one: length* = 1 and S_OK; none: X_E_NO_MORE_FILES (80070012).
    // An extra without the signature, an enumerator that is not an aggregate one, or a buffer smaller than an item stops (7).
    const uint32_t extra = Load32(base, data + 8), buffer = Load32(base, data + 12), size = Load32(base, data + 16),
                   lengthOut = Load32(base, data + 24);
    if (!buffer || !extra)
        return kInvalidArg;
    if (Load32(base, extra) != 0x58454E00)
        Stop(StopCode::Unimplemented, "XamAppEnumerateContentAggregate: extra %08X has no XEN signature", extra);
    const auto object = LookupHandle(Load32(base, extra + 4));
    std::lock_guard guard(enumLock);
    const auto it = object && object->type == ObjectType::Enumerator ? enumerators.find(object->header) : enumerators.end();
    if (it == enumerators.end())
        return kInvalidArg;
    Enumerator& e = it->second;
    if (e.itemSize != kContentDataSize + 4 || Load32(base, object->header + 4) != 0x2000E)
        Stop(StopCode::Unimplemented, "XamAppEnumerateContentAggregate on an enumerator that is not an aggregate one");
    if (size)
        std::memset(base + buffer, 0, size);
    if (e.next == e.items.size())
        return (1u << 31) | (7u << 16) | kErrorNoMoreFiles;
    if (size < e.itemSize)
        Stop(StopCode::Unimplemented, "XamAppEnumerateContentAggregate: a %u-byte buffer for a %u-byte item", size, e.itemSize);
    std::memcpy(base + buffer, e.items[e.next].data(), e.itemSize);
    e.next++;
    if (lengthOut)
        Store32(base, lengthOut, 1);
    return 0;
}

GUEST_IMPORT(XamGetPrivateEnumStructureFromHandle, "REFERENCE_DERIVED: Xenia xam_enum.cc:107-124 (the enumerator's guest object)")
{
    // (handle, object*): an enumerator handle: object* = its X_KENUMERATOR (the guest object), 0. Otherwise
    // STATUS_INVALID_HANDLE. Xenia takes a reference for ObDereferenceObject to release; objects live for the whole run
    // here (ObReferenceObjectByHandle), so none is counted.
    using namespace runtime;
    const uint32_t handle = ctx.r3.u32, out = ctx.r4.u32;
    const auto object = LookupHandle(handle);
    if (!object || object->type != ObjectType::Enumerator)
        return (void)(ctx.r3.u64 = status::kInvalidHandle);
    if (out)
        Store32(base, out, object->header);
    ctx.r3.u64 = 0;
}
