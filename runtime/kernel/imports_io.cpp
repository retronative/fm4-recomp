// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// File-system imports. Semantics: Xenia xenia-canary 67f16c4e315f,
// src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc.
#include "imports.h"
#include "diag_waits.h"  // FM4 Recomp: FM4_DIAG_WAITS (diagnostic)
#include "file_system.h"
#include <algorithm>
#include "content.h"
#include "objects.h"
#include <optional>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include "dispatcher.h"
#include <string>
#include <string_view>

GUEST_IMPORT(FscSetCacheElementCount, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:669 FscSetCacheElementCount (a stub: success)")
{
    // (0, count): sizes the kernel's file-system cache. The runtime has no such cache, so the setting has
    // no guest-visible effect; Xenia also returns success. Policy: accepted only
    // in the shape FM4 uses (first argument 0, a count of 1..65536 elements); anything else stops (7),
    // because its meaning is undocumented (Xenia's own comment: "unk_1 looks like a count?").
    (void)base;
    const uint32_t first = ctx.r3.u32, count = ctx.r4.u32;
    if (first != 0 || count == 0 || count > 0x10000)
        Stop(StopCode::Unimplemented, "FscSetCacheElementCount(%u, %u): arguments outside the modelled shape", first, count);
    ctx.r3.u64 = 0;  // X_STATUS_SUCCESS
}

namespace {

// The path an X_OBJECT_ATTRIBUTES names (Xenia xbox.h:314: root directory +0, ANSI_STRING* +4): stops (7)
// for a root directory other than none (0) or ObDosDevices (0xFFFFFFFD), as Xenia asserts on it.
std::string ObjectPath(const PPCContext& ctx, const uint8_t* base, uint32_t attributes, const char* import)
{
    const uint32_t root = runtime::Load32(base, attributes);
    if (root != 0 && root != 0xFFFFFFFD)
        Stop(StopCode::Unimplemented, "%s: a path relative to handle %08X is not implemented (lr=%08X)", import, root, uint32_t(ctx.lr));
    const uint32_t name = runtime::Load32(base, attributes + 4);
    const uint32_t length = uint32_t(base[name] << 8 | base[name + 1]), buffer = runtime::Load32(base, name + 4);
    return std::string(reinterpret_cast<const char*>(base + buffer), length);
}

// X_FILE_NETWORK_OPEN_INFORMATION (Xenia info/file.h:98): four times, allocation size, size (big-endian
// 64 bits each), attributes, padding: 56 bytes.
constexpr uint32_t kNetworkOpenInformationSize = 56;
void WriteNetworkOpenInformation(uint8_t* base, uint32_t info, const runtime::DiscEntry& entry)
{
    uint64_t times[4];
    runtime::DiscTimes(entry, times);
    const uint64_t values[6] = {times[0], times[1], times[2], times[3], runtime::DiscAllocationSize(entry), entry.size};
    for (int i = 0; i < 6; i++)
    {
        runtime::Store32(base, info + 8 * i, uint32_t(values[i] >> 32));
        runtime::Store32(base, info + 8 * i + 4, uint32_t(values[i]));
    }
    runtime::Store32(base, info + 48, runtime::DiscAttributes(entry));
    runtime::Store32(base, info + 52, 0);
}

}  // namespace

GUEST_IMPORT(NtQueryFullAttributesFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:505 NtQueryFullAttributesFile, info/file.h:98")
{
    // (attributes*, X_FILE_NETWORK_OPEN_INFORMATION*): a path on the disc gives its times, allocation
    // size, size and attributes (big-endian 64-bit x 6, then 32-bit attributes and padding; 56 bytes).
    // A non-ASCII or reserved character: OBJECT_NAME_INVALID; a missing file on the disc: NO_SUCH_FILE;
    // a path on any other device stops (7), naming it (no other device is mounted yet).
    const std::string path = ObjectPath(ctx, base, ctx.r3.u32, "NtQueryFullAttributesFile");
    const uint32_t info = ctx.r4.u32;
    if (!runtime::ValidGuestPath(path))
    {
        ctx.r3.u64 = 0xC0000033;  // STATUS_OBJECT_NAME_INVALID
        return;
    }
    runtime::DiscEntry entry;
    std::string device;
    runtime::Resolved resolved = runtime::ResolveGuestPath(path, entry, device);
    if (resolved == runtime::Resolved::OtherDevice && runtime::TitleUpdatePath(device))
        resolved = runtime::Resolved::NotFound;  // no title update installed (file_system.h)
    switch (resolved)
    {
    case runtime::Resolved::OtherDevice:
        Stop(StopCode::Unimplemented, "NtQueryFullAttributesFile(\"%s\" = %s): not on the disc, and no other device is mounted",
             path.c_str(), device.c_str());
    case runtime::Resolved::NotFound:
        ctx.r3.u64 = 0xC000000F;  // STATUS_NO_SUCH_FILE
        return;
    case runtime::Resolved::Found:
        break;
    }
    WriteNetworkOpenInformation(base, info, entry);
    ctx.r3.u64 = 0;
}

namespace {

// Access bits that would change the disc (NT FILE_WRITE_DATA, FILE_APPEND_DATA, FILE_WRITE_EA,
// FILE_WRITE_ATTRIBUTES, DELETE, WRITE_DAC, WRITE_OWNER, GENERIC_ALL, GENERIC_WRITE).
constexpr uint32_t kWriteAccess = 0x2 | 0x4 | 0x10 | 0x100 | 0x10000 | 0x40000 | 0x80000 | 0x10000000 | 0x40000000;
constexpr uint32_t kOpen = 1, kOpenIf = 3;                       // FileDisposition (Xenia vfs/entry.h:47)
constexpr uint32_t kOpened = 1, kDoesNotExist = 5;               // FileAction (Xenia vfs/entry.h:38)
constexpr uint32_t kNonDirectoryFile = 0x40;                     // FILE_NON_DIRECTORY_FILE
constexpr uint32_t kSynchronousIo = 0x10 | 0x20;
constexpr std::string_view kHardDisk = "\\Device\\Harddisk0";  // absent (owner policy, below)                 // FILE_SYNCHRONOUS_IO_ALERT, _NONALERT

// FM4 Recomp: one line per STATUS_DISK_FULL returned to the game, with the figures behind it (content.h ContentFree).
void LogDiskFull(const char* import, const std::string& host, uint64_t growth)
{
    std::fprintf(stderr, "[fs] %s(%s): STATUS_DISK_FULL: %llu more byte(s) wanted; saved content has %llu byte(s) free (the Mac's volume has "
                         "%llu available, of which %llu are kept in reserve; %llu of the device's %llu stored)\n",
                 import, host.c_str(), (unsigned long long)growth, (unsigned long long)runtime::ContentFreeBytes(),
                 (unsigned long long)runtime::HostAvailableBytes(), (unsigned long long)runtime::kHostReserve,
                 (unsigned long long)runtime::ContentBytesUsed(), (unsigned long long)runtime::kContentCapacity);
}

// NtCreateFile/NtOpenFile on the read-only disc (Xenia xboxkrnl_io.cc:98, virtual_file_system.cc:181).
void OpenOnDisc(PPCContext& ctx, uint8_t* base, const char* import, uint32_t handleOut, uint32_t access, uint32_t attributes,
                uint32_t ioStatus, uint32_t disposition, uint32_t options)
{
    auto finish = [&](uint32_t status, uint32_t action, uint32_t handle) {
        if (ioStatus)
            runtime::Store32(base, ioStatus, status), runtime::Store32(base, ioStatus + 4, action);
        runtime::Store32(base, handleOut, handle);
        ctx.r3.u64 = status;
    };
    if (attributes == 0)
        return (void)(ctx.r3.u64 = 0xC000000D);  // STATUS_INVALID_PARAMETER (Xenia: before the handle)
    const std::string path = ObjectPath(ctx, base, attributes, import);
    if (!runtime::ValidGuestPath(path))
        return (void)(ctx.r3.u64 = 0xC0000033);  // STATUS_OBJECT_NAME_INVALID
    runtime::DiscEntry entry;
    std::string device;
    const runtime::Resolved resolved = runtime::ResolveGuestPath(path, entry, device);
    // A compatibility policy, not verified console behaviour: the console has no
    // hard disk. An open on \Device\Harddisk0 fails with STATUS_OBJECT_NAME_NOT_FOUND, the output handle
    // X_INVALID_HANDLE_VALUE and the io status {status, FILE_DOES_NOT_EXIST}, as Xenia's failed opens
    // (xboxkrnl_io.cc:150-170); nothing is read or written. Xenia's NullDevice (success for all I/O,
    // emulator.cc:910-924) is not used.
    if (resolved == runtime::Resolved::OtherDevice && device.compare(0, kHardDisk.size(), kHardDisk) == 0 &&
        (device.size() == kHardDisk.size() || device[kHardDisk.size()] == '\\'))
        return finish(0xC0000034, kDoesNotExist, 0xFFFFFFFF);  // STATUS_OBJECT_NAME_NOT_FOUND
    if (resolved == runtime::Resolved::OtherDevice && runtime::TitleUpdatePath(device))
        return finish(0xC000000F, kDoesNotExist, 0xFFFFFFFF);  // no title update installed: STATUS_NO_SUCH_FILE
    if (resolved == runtime::Resolved::OtherDevice)  // first, so the stop names the real reason
        Stop(StopCode::Unimplemented, "%s(\"%s\" = %s, access %08X, disposition %u): not on the disc, and no other device is mounted",
             import, path.c_str(), device.c_str(), access, disposition);
    if (entry.writable)
    {
        // A writable content root (saved content): Xenia virtual_file_system.cc:181-329 OpenFile on a writable
        // HostPathDevice. OPEN and OVERWRITE need the entry (NO_SUCH_FILE, FILE_DOES_NOT_EXIST), CREATE needs it absent
        // (OBJECT_NAME_COLLISION 0xC0000035, FILE_EXISTS 4); a missing parent NO_SUCH_FILE. A missing entry is created
        // (FILE_DIRECTORY_FILE (1): a directory) with FILE_CREATED (2); an existing one opens (FILE_OPENED) or, for
        // SUPERSEDE (0) and OVERWRITE(_IF) (4, 5), is emptied (FILE_SUPERSEDED 0, FILE_OVERWRITTEN 3; Xenia deletes and
        // recreates). A host failure: ACCESS_DENIED (0xC0000022), as Xenia's failed create or delete.
        constexpr uint32_t kSupersede = 0, kCreate = 2, kOverwrite = 4, kOverwriteIf = 5, kDirectoryFile = 0x1;
        constexpr uint32_t kSuperseded = 0, kCreated = 2, kOverwritten = 3, kExists = 4;
        if (disposition > kOverwriteIf)
            Stop(StopCode::Unimplemented, "%s(\"%s\"): disposition %u is not a file disposition", import, path.c_str(), disposition);
        const bool found = resolved == runtime::Resolved::Found;
        if (!found && entry.host.empty())
            return finish(0xC000000F, kDoesNotExist, 0xFFFFFFFF);  // the parent is missing
        if (found && entry.directory && (options & kNonDirectoryFile))
            return finish(0xC00000BA, 0, 0xFFFFFFFF);
        if (!found && (disposition == kOpen || disposition == kOverwrite))
            return finish(0xC000000F, kDoesNotExist, 0xFFFFFFFF);
        if (found && disposition == kCreate)
            return finish(0xC0000035, kExists, 0xFFFFFFFF);  // STATUS_OBJECT_NAME_COLLISION
        uint32_t action = kOpened;
        if (!found)
        {
            const bool directory = (options & kDirectoryFile) != 0;
            if (!runtime::CreateHostEntry(entry.host, directory))
            {
                // FM4 Recomp: a full host volume (mkdir/open ENOSPC, file_system.h HostVolumeFull) is STATUS_DISK_FULL (NT; Xenia
                // has one status for every failed create); anything else ACCESS_DENIED as before.
                const bool full = runtime::HostVolumeFull(errno);
                if (full)
                    LogDiskFull(import, entry.host, 0);
                return finish(full ? 0xC000007F : 0xC0000022, kDoesNotExist, 0xFFFFFFFF);  // STATUS_DISK_FULL : STATUS_ACCESS_DENIED
            }
            entry.directory = directory, entry.size = 0, entry.attributes = directory ? runtime::file_attribute::kDirectory : 0x80;
            action = kCreated;
        }
        else if (disposition == kSupersede || disposition == kOverwrite || disposition == kOverwriteIf)
        {
            if (entry.directory)
                Stop(StopCode::Unimplemented, "%s(\"%s\", disposition %u): replacing a directory is not modelled", import, path.c_str(), disposition);
            if (!runtime::TruncateHostFile(entry.host))
                return finish(0xC0000022, kDoesNotExist, 0xFFFFFFFF);
            entry.size = 0;
            action = disposition == kSupersede ? kSuperseded : kOverwritten;
        }
        std::fprintf(stderr, "[fs] %s(\"%s\", access %08X, disposition %u): %s in the writable content root\n", import, path.c_str(), access,
                     disposition, action == kCreated ? "created" : action == kOpened ? "opened" : "emptied");
        return finish(0, action, runtime::OpenDiscFile(base, entry, (options & kSynchronousIo) != 0));
    }
    if ((access & kWriteAccess) || (disposition != kOpen && disposition != kOpenIf))
        Stop(StopCode::Unimplemented, "%s(\"%s\", access %08X, disposition %u): the disc is read-only; writing is not modelled",
             import, path.c_str(), access, disposition);
    switch (resolved)
    {
    case runtime::Resolved::OtherDevice:
        break;  // stopped above
    case runtime::Resolved::NotFound:
        if (disposition == kOpenIf)
            Stop(StopCode::Unimplemented, "%s(\"%s\", OPEN_IF): would create a file on the read-only disc", import, path.c_str());
        return finish(0xC000000F, kDoesNotExist, 0xFFFFFFFF);  // STATUS_NO_SUCH_FILE, X_INVALID_HANDLE_VALUE
    case runtime::Resolved::Found:
        break;
    }
    if (entry.directory && (options & kNonDirectoryFile))
        return finish(0xC00000BA, 0, 0xFFFFFFFF);  // STATUS_FILE_IS_A_DIRECTORY (no action written by Xenia: 0 here)
    runtime::diag::Opened(path.c_str());  // FM4_DIAG_WAITS (diagnostic)
    finish(0, kOpened, runtime::OpenDiscFile(base, entry, (options & kSynchronousIo) != 0));
}

}  // namespace

GUEST_IMPORT(NtCreateFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:98 NtCreateFile, virtual_file_system.cc:181 OpenFile")
{
    // (handle*, access, attributes*, io status*, allocation size*, file attributes, share, disposition, options).
    OpenOnDisc(ctx, base, "NtCreateFile", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32,
               runtime::Argument(ctx, base, 8));
}

GUEST_IMPORT(NtOpenFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:173 NtOpenFile (NtCreateFile with FILE_OPEN)")
{
    // (handle*, access, attributes*, io status*, options).
    OpenOnDisc(ctx, base, "NtOpenFile", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, kOpen, ctx.r7.u32);
}

GUEST_IMPORT(NtQueryDirectoryFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:547-588 NtQueryDirectoryFile, kernel/xfile.cc:43-94 QueryDirectory, xfile.h:36")
{
    // (handle, event, APC routine, APC context, io status*, X_FILE_DIRECTORY_INFORMATION*, length, ANSI file name*,
    // restart scan): one child of the directory per call, as Xenia. A file name sets the pattern and restarts the
    // scan (Xenia: "Always restart the search?"); without one the scan goes on, from the start if `restart`. The record
    // (big-endian): next 0, index (the child's 1-based position), four times, size, allocation size, attributes, name
    // length, then the name (not terminated). Xenia's order: length < 72 INFO_LENGTH_MISMATCH; an invalid pattern
    // INVALID_PARAMETER; a handle that is no file NO_SUCH_FILE; nothing left NO_MORE_FILES, or NO_SUCH_FILE for a file
    // name with no match or a name past the buffer. The io status gets {status, length on success, else 0}. An event or
    // APC (completion through them) stops (7), as does a pattern with a path separator (Xenia asserts on it).
    const uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, ioStatus = ctx.r7.u32, info = ctx.r8.u32,
                   length = ctx.r9.u32, fileName = ctx.r10.u32, restart = runtime::Argument(ctx, base, 8);
    if (event != 0 || apc != 0)
        Stop(StopCode::Unimplemented, "NtQueryDirectoryFile(%08X): completion through event %08X or APC %08X is not modelled", handle, event, apc);
    if (length < 72)
        return (void)(ctx.r3.u64 = 0xC0000004);  // STATUS_INFO_LENGTH_MISMATCH
    std::string name;
    if (fileName != 0)
    {
        const uint32_t size = uint32_t(base[fileName] << 8 | base[fileName + 1]), buffer = runtime::Load32(base, fileName + 4);
        name.assign(reinterpret_cast<const char*>(base + buffer), size);
    }
    if (!runtime::ValidGuestPath(name, true))
        return (void)(ctx.r3.u64 = 0xC000000D);  // STATUS_INVALID_PARAMETER
    if (name.find('\\') != std::string::npos)
        Stop(StopCode::Unimplemented, "NtQueryDirectoryFile(%08X, \"%s\"): a pattern with a path is not modelled", handle, name.c_str());
    const auto object = runtime::LookupHandle(handle);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    uint32_t status = 0;
    if (file == nullptr)
        status = 0xC000000F;  // STATUS_NO_SUCH_FILE
    else
    {
        if (!file->listed)
            file->children = runtime::ListDirectory(file->entry), file->listed = true;
        if (!name.empty())
        {
            file->findPattern = name, file->findIndex = 0;
            std::fprintf(stderr, "[fs] NtQueryDirectoryFile(%s, \"%s\"): %zu children\n",
                         file->entry.package ? "a package directory" : ("disc \\" + file->entry.relative).c_str(), name.c_str(), file->children.size());
        }
        else if (restart != 0)
            file->findIndex = 0;
        const runtime::DirectoryChild* child = nullptr;
        while (child == nullptr && file->findIndex < file->children.size())
        {
            const runtime::DirectoryChild& c = file->children[file->findIndex++];
            if (runtime::WildcardMatch(file->findPattern, c.name))
                child = &c;
        }
        if (child == nullptr || 0x40 + child->name.size() > length)
            status = child == nullptr && name.empty() ? (1u << 31 | 0x6) : 0xC000000F;  // STATUS_NO_MORE_FILES : STATUS_NO_SUCH_FILE
        else
        {
            uint64_t times[4];
            runtime::DiscTimes(child->entry, times);
            const uint64_t values[6] = {times[0], times[1], times[2], times[3], child->entry.size, runtime::DiscAllocationSize(child->entry)};
            runtime::Store32(base, info, 0);
            runtime::Store32(base, info + 4, uint32_t(file->findIndex));
            for (int i = 0; i < 6; i++)
                runtime::Store32(base, info + 8 + 8 * i, uint32_t(values[i] >> 32)), runtime::Store32(base, info + 12 + 8 * i, uint32_t(values[i]));
            runtime::Store32(base, info + 0x38, runtime::DiscAttributes(child->entry));
            runtime::Store32(base, info + 0x3C, uint32_t(child->name.size()));
            std::memcpy(base + info + 0x40, child->name.data(), child->name.size());
        }
    }
    if (ioStatus)
        runtime::Store32(base, ioStatus, status), runtime::Store32(base, ioStatus + 4, status == 0 ? length : 0);
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtWriteFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:354-429 NtWriteFile, xfile.cc:239-268 XFile::Write; compatibility policy: save storage (quota)")
{
    // (handle, event, APC routine, APC context, io status*, buffer*, length, offset*), completed at once as Xenia and as
    // NtReadFile here: a bad event or file handle INVALID_HANDLE with io status {status, 0}; the write at *offset, or at
    // the position without one (or -1); io status {0, bytes}, the event set, PENDING for an asynchronous file. A
    // synchronous file's position becomes offset + bytes (NT; as NtReadFile); an asynchronous file's advances by the bytes
    // (Xenia XFile::Write, xfile.cc:236-246 at 67f16c4: no offset writes at position_). Free space (content.h ContentFreeBytes: the
    // 20 GB device minus what is stored, bounded by the host volume minus its reserve; Xenia checks none): a write growing
    // stored content past it fails with STATUS_DISK_FULL (C000007F), io status {status, 0}, nothing written. A file outside a writable content root (the disc, a package) or an APC stops (7).
    const uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, ioStatus = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, offsetAt = ctx.r10.u32;
    auto fail = [&](uint32_t status) {
        if (ioStatus)
            runtime::Store32(base, ioStatus, status), runtime::Store32(base, ioStatus + 4, 0);
        ctx.r3.u64 = status;
    };
    std::optional<runtime::KernelObject> eventObject;
    if (event != 0)
    {
        eventObject = runtime::LookupHandle(event);
        if (!eventObject || eventObject->type != runtime::ObjectType::Event)
            return fail(runtime::status::kInvalidHandle);
    }
    const auto object = runtime::LookupHandle(handle);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    if (file == nullptr)
        return fail(runtime::status::kInvalidHandle);
    if ((apc & ~1u) != 0)
        Stop(StopCode::Unimplemented, "NtWriteFile(%08X): an APC routine (%08X) is not implemented", handle, apc);
    if (!file->entry.writable || file->entry.directory)
        Stop(StopCode::Unimplemented, "NtWriteFile(%08X): %s is not a file in a writable content root (writing the disc or a package is not modelled)",
             handle, file->entry.host.c_str());
    uint64_t offset = file->position;
    if (offsetAt != 0)
    {
        const uint64_t given = uint64_t(runtime::Load32(base, offsetAt)) << 32 | runtime::Load32(base, offsetAt + 4);
        if (given != ~uint64_t(0))
            offset = given;
    }
    const uint64_t size = runtime::HostFileSize(file->entry), end = offset + length;
    const uint64_t growth = length != 0 && end > size ? end - size : 0;
    // FM4 Recomp (Disc 2): the device's and the host's free space (content.h). A refusal is logged: without the line a write the
    // game was refused leaves no trace in the log. The host volume filling
    // under the write itself (ENOSPC, file_system.h HostVolumeFull) is the same status, not a stop.
    if (growth != 0 && growth > runtime::ContentFreeBytes())
    {
        LogDiskFull("NtWriteFile", file->entry.host, growth);
        return fail(0xC000007F);  // STATUS_DISK_FULL
    }
    if (length != 0 && !runtime::WriteHostFile(file->entry, offset, base + buffer, length))
    {
        LogDiskFull("NtWriteFile (the host write)", file->entry.host, growth);
        return fail(0xC000007F);
    }
    const uint32_t bytes = length;
    file->entry.size = std::max(size, offset + bytes);
    file->position = file->synchronous ? offset + bytes : file->position + bytes;
    if (ioStatus)
        runtime::Store32(base, ioStatus, 0), runtime::Store32(base, ioStatus + 4, bytes);
    if (eventObject)
        runtime::SetEvent(base, eventObject->header);
    runtime::SetEvent(base, object->header);  // the file object itself (Xenia XFile::Write, xfile.cc:305: its async event)
    ctx.r3.u64 = file->synchronous ? 0 : 0x103;  // STATUS_PENDING
}

GUEST_IMPORT(NtFlushBuffersFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:590-601 NtFlushBuffersFile (success, io status {0, 0})")
{
    // (handle, io status*): Xenia returns success for any handle. Writes here reach the host file at once (NtWriteFile),
    // so a file in a writable content root is fsync'ed and the flush succeeds; a read-only file has nothing to flush.
    // The io status gets {0, 0}. A handle that is no file stops (7) (Xenia does not check; success would be invented).
    const uint32_t handle = ctx.r3.u32, ioStatus = ctx.r4.u32;
    const auto object = runtime::LookupHandle(handle);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    if (file == nullptr)
        Stop(StopCode::Unimplemented, "NtFlushBuffersFile(%08X): not a file handle", handle);
    if (file->entry.writable && !file->entry.directory)
        runtime::SyncHostFile(file->entry);
    if (ioStatus)
        runtime::Store32(base, ioStatus, 0), runtime::Store32(base, ioStatus + 4, 0);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NtQueryInformationFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io_info.cc:58 NtQueryInformationFile, info/file.h:19")
{
    // (handle, io status*, info*, length, class): FilePositionInformation (14: the position, big-endian
    // 64 bits) and FileNetworkOpenInformation (34: 56 bytes). Xenia's order: the class's minimum length
    // (INFO_LENGTH_MISMATCH), the handle (INVALID_HANDLE), the buffer zeroed, then the io status
    // {status, bytes written}. Any other class stops (7).
    const uint32_t handle = ctx.r3.u32, ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    // FileSectorInformation (26): the file's first sector in the disc volume (from the disc map). Xenia returns a hash
    // of the path (io_info.cc:94-104, "used to identify files"); INFERRED from the class's name.
    constexpr uint32_t kPosition = 14, kSector = 26, kNetworkOpen = 34;
    if (infoClass != kPosition && infoClass != kSector && infoClass != kNetworkOpen)
        Stop(StopCode::Unimplemented, "NtQueryInformationFile: information class %u is not implemented", infoClass);
    const uint32_t needed = infoClass == kPosition ? 8 : infoClass == kSector ? 4 : kNetworkOpenInformationSize;
    if (length < needed)
        return (void)(ctx.r3.u64 = 0xC0000004);  // STATUS_INFO_LENGTH_MISMATCH
    const auto object = runtime::LookupHandle(handle);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    if (file == nullptr)
        return (void)(ctx.r3.u64 = runtime::status::kInvalidHandle);
    if (file->entry.writable && !file->entry.directory)
        file->entry.size = runtime::HostFileSize(file->entry);  // another handle may have written since
    std::memset(base + info, 0, length);
    if (infoClass == kSector)
        runtime::Store32(base, info, file->entry.sector);
    else if (infoClass == kPosition)
    {
        runtime::Store32(base, info, uint32_t(file->position >> 32));
        runtime::Store32(base, info + 4, uint32_t(file->position));
    }
    else
        WriteNetworkOpenInformation(base, info, file->entry);
    if (ioStatus)
        runtime::Store32(base, ioStatus, 0), runtime::Store32(base, ioStatus + 4, needed);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NtSetInformationFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io_info.cc:186-262 NtSetInformationFile (XFilePositionInformation, XFileAllocationInformation ignored, XFileEndOfFileInformation -> XFile::SetLength)")
{
    // (handle, io status*, info*, length, class): FilePositionInformation (14) sets the position, FileEndOfFileInformation
    // (20) the file's length (a writable saved-content file: cut or zero-extended on the host), each from big-endian 64
    // bits. FileAllocationInformation (19): Xenia ignores it (:222-226, success); here so does an allocation at or above the
    // file's size (space is taken on write, where free space is checked), while one below it, which NT would cut the file
    // to, stops (7). FileDispositionInformation (13, 1 byte): Xenia ignores it (:207-214); here, as NT, a writable
    // saved-content file is removed when this handle closes (NtClose) if the byte is non-zero (io status {0, 0}); a disc
    // file or a directory stops (7). Xenia's order: a length below the class's minimum (8) is INFO_LENGTH_MISMATCH, an unknown handle
    // INVALID_HANDLE; then the io status {status, 8} and the status. Growing stored content past the free space
    // (content.h ContentFreeBytes) is STATUS_DISK_FULL with nothing changed, as NtWriteFile. The length of a disc file, and any other class, stop (7):
    // disposition (deletion) and allocation are not modelled until a run needs them.
    const uint32_t handle = ctx.r3.u32, ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    constexpr uint32_t kDisposition = 13, kPosition = 14, kAllocation = 19, kEndOfFile = 20;
    if (infoClass != kDisposition && infoClass != kPosition && infoClass != kAllocation && infoClass != kEndOfFile)
        Stop(StopCode::Unimplemented, "NtSetInformationFile: information class %u is not implemented", infoClass);
    if (length < (infoClass == kDisposition ? 1u : 8u))
        return (void)(ctx.r3.u64 = 0xC0000004);  // STATUS_INFO_LENGTH_MISMATCH
    const auto object = runtime::LookupHandle(handle);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    if (file == nullptr)
        return (void)(ctx.r3.u64 = runtime::status::kInvalidHandle);
    if (infoClass == kDisposition)
    {
        // FM4 Recomp: a saved-content directory too (FM4 overwrites a photo by deleting its three files and then its folder): it is
        // removed when this handle closes, once its files are gone (RemoveHostFile). A disc file stops (7).
        if (!file->entry.writable)
            Stop(StopCode::Unimplemented, "NtSetInformationFile(%08X): delete-on-close of a disc file", handle);
        file->deleteOnClose = base[info] != 0;
        if (ioStatus)
            runtime::Store32(base, ioStatus, 0), runtime::Store32(base, ioStatus + 4, 0);
        return (void)(ctx.r3.u64 = 0);
    }
    const uint64_t value = uint64_t(runtime::Load32(base, info)) << 32 | runtime::Load32(base, info + 4);
    uint32_t status = 0;
    if (infoClass == kPosition)
        file->position = value;
    else if (infoClass == kAllocation)
    {
        const uint64_t size = file->entry.writable && !file->entry.directory ? runtime::HostFileSize(file->entry) : file->entry.size;
        if (value < size)
            Stop(StopCode::Unimplemented, "NtSetInformationFile(%08X): an allocation of %llu bytes below the file's %llu would cut it", handle,
                 (unsigned long long)value, (unsigned long long)size);
    }
    else
    {
        if (!file->entry.writable || file->entry.directory)
            Stop(StopCode::Unimplemented, "NtSetInformationFile(%08X): setting the length of a file outside a writable content root", handle);
        const uint64_t size = runtime::HostFileSize(file->entry);
        if (value > size && value - size > runtime::ContentFreeBytes())  // FM4 Recomp (Disc 2): the device's free space (content.h)
            status = 0xC000007F;  // STATUS_DISK_FULL
        else if (!runtime::SetHostFileLength(file->entry, value))  // FM4 Recomp: the host volume filled (file_system.h HostVolumeFull)
            status = 0xC000007F;
        else
            file->entry.size = value;
        if (status != 0)
            LogDiskFull("NtSetInformationFile(end of file)", file->entry.host, value > size ? value - size : 0);
    }
    if (ioStatus)
        runtime::Store32(base, ioStatus, status), runtime::Store32(base, ioStatus + 4, 8);
    ctx.r3.u64 = status;
}

GUEST_IMPORT(NtReadFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:184 NtReadFile, xfile.cc:96 XFile::Read, disc_image_file.cc:26")
{
    // (handle, event, APC routine, APC context, io status*, buffer*, length, offset*). Xenia completes every
    // read at once: the io status gets {status, bytes}, the event (if any) is set, and a file opened for
    // asynchronous I/O returns PENDING unless the read hit the end (completing before returning is a valid
    // order for asynchronous I/O). The read is at *offset, or at the file position if there is no offset (or
    // it is -1); at or past the end: END_OF_FILE; a zero length succeeds with 0 bytes. A synchronous file's
    // position becomes offset + bytes (NT's rule; Xenia adds the bytes to the old position). An asynchronous
    // file keeps a position too, as Xenia's XFile::Read (xfile.cc:94-158 at 67f16c4: no offset reads at
    // position_, which advances by the bytes read on success); FM4's Disc 2 install reads its packages that
    // way. An APC stops (7).
    const uint32_t handle = ctx.r3.u32, event = ctx.r4.u32, apc = ctx.r5.u32, ioStatus = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32, length = ctx.r9.u32, offsetAt = ctx.r10.u32;
    std::optional<runtime::KernelObject> eventObject;
    if (event != 0)
    {
        eventObject = runtime::LookupHandle(event);
        if (!eventObject || eventObject->type != runtime::ObjectType::Event)
            return (void)(ctx.r3.u64 = runtime::status::kInvalidHandle);
    }
    const auto object = runtime::LookupHandle(handle);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    if (file == nullptr)
        return (void)(ctx.r3.u64 = runtime::status::kInvalidHandle);
    if ((apc & ~1u) != 0)
        Stop(StopCode::Unimplemented, "NtReadFile(%08X): an APC routine (%08X) is not implemented", handle, apc);
    uint64_t offset = file->position;
    if (offsetAt != 0)
    {
        const uint64_t given = uint64_t(runtime::Load32(base, offsetAt)) << 32 | runtime::Load32(base, offsetAt + 4);
        if (given != ~uint64_t(0))
            offset = given;
    }
    if (file->entry.writable && !file->entry.directory)
        file->entry.size = runtime::HostFileSize(file->entry);  // another handle may have written since
    uint32_t status = 0, bytes = 0;
    if (length != 0)
    {
        if (offset >= file->entry.size)
            status = 0xC0000011;  // STATUS_END_OF_FILE
        else
            bytes = runtime::ReadDiscFile(file->entry, offset, base + buffer, length);
        runtime::diag::ReadBytes(bytes);  // FM4_DIAG_WAITS (diagnostic)
    }
    if (status == 0)
        file->position = file->synchronous ? offset + bytes : file->position + bytes;
    if (ioStatus)
        runtime::Store32(base, ioStatus, status), runtime::Store32(base, ioStatus + 4, bytes);
    if (eventObject)
        runtime::SetEvent(base, eventObject->header);
    runtime::SetEvent(base, object->header);  // the file object itself (Xenia XFile::Read, xfile.cc:185: its async event)
    ctx.r3.u64 = !file->synchronous && status == 0 ? 0x103 : status;  // STATUS_PENDING
}
