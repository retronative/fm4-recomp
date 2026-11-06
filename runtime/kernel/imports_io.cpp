// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// File-system imports. Semantics: Xenia xenia-canary 67f16c4e315f,
// src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc.
#include "imports.h"
#include "file_system.h"
#include "objects.h"
#include <optional>
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
    const uint64_t values[6] = {runtime::kDiscTimeStamp, runtime::kDiscTimeStamp, runtime::kDiscTimeStamp,
                                runtime::kDiscTimeStamp, runtime::DiscAllocationSize(entry), entry.size};
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

GUEST_IMPORT(NtSetInformationFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io_info.cc:186-245 NtSetInformationFile (XFilePositionInformation)")
{
    // (handle, io status*, info*, length, class): FilePositionInformation (14) sets the position from its big-endian
    // 64 bits. Xenia's order: a length below the class's minimum (8) is INFO_LENGTH_MISMATCH, an unknown handle
    // INVALID_HANDLE; then the io status {0, 8} and 0. Any other class stops (7): disposition (deletion) and
    // allocation are writes to a read-only disc that Xenia ignores, not modelled until a run needs them.
    const uint32_t handle = ctx.r3.u32, ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    constexpr uint32_t kPosition = 14;
    if (infoClass != kPosition)
        Stop(StopCode::Unimplemented, "NtSetInformationFile: information class %u is not implemented", infoClass);
    if (length < 8)
        return (void)(ctx.r3.u64 = 0xC0000004);  // STATUS_INFO_LENGTH_MISMATCH
    const auto object = runtime::LookupHandle(handle);
    runtime::OpenFile* file = object && object->type == runtime::ObjectType::File ? runtime::FindOpenFile(object->header) : nullptr;
    if (file == nullptr)
        return (void)(ctx.r3.u64 = runtime::status::kInvalidHandle);
    file->position = uint64_t(runtime::Load32(base, info)) << 32 | runtime::Load32(base, info + 4);
    if (ioStatus)
        runtime::Store32(base, ioStatus, 0), runtime::Store32(base, ioStatus + 4, 8);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(NtReadFile, "REFERENCE_DERIVED: Xenia xboxkrnl_io.cc:184 NtReadFile, xfile.cc:96 XFile::Read, disc_image_file.cc:26")
{
    // (handle, event, APC routine, APC context, io status*, buffer*, length, offset*). Xenia completes every
    // read at once: the io status gets {status, bytes}, the event (if any) is set, and a file opened for
    // asynchronous I/O returns PENDING unless the read hit the end (completing before returning is a valid
    // order for asynchronous I/O). The read is at *offset, or at the file position if there is no offset (or
    // it is -1); at or past the end: END_OF_FILE; a zero length succeeds with 0 bytes. A synchronous file's
    // position becomes offset + bytes (NT's rule; Xenia adds the bytes to the old position). NT keeps no
    // position for asynchronous files, so such a read without an offset stops (7); so does an APC.
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
    bool explicitOffset = false;
    if (offsetAt != 0)
    {
        const uint64_t given = uint64_t(runtime::Load32(base, offsetAt)) << 32 | runtime::Load32(base, offsetAt + 4);
        if (given != ~uint64_t(0))
            offset = given, explicitOffset = true;
    }
    if (!file->synchronous && !explicitOffset)
        Stop(StopCode::Unimplemented, "NtReadFile(%08X): an asynchronous read without an offset", handle);
    uint32_t status = 0, bytes = 0;
    if (length != 0)
    {
        if (offset >= file->entry.size)
            status = 0xC0000011;  // STATUS_END_OF_FILE
        else
            bytes = runtime::ReadDiscFile(file->entry, offset, base + buffer, length);
    }
    if (status == 0 && file->synchronous)
        file->position = offset + bytes;
    if (ioStatus)
        runtime::Store32(base, ioStatus, status), runtime::Store32(base, ioStatus + 4, bytes);
    if (eventObject)
        runtime::SetEvent(base, eventObject->header);
    ctx.r3.u64 = !file->synchronous && status == 0 ? 0x103 : status;  // STATUS_PENDING
}
