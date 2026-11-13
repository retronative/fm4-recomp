// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Saved content: isolated, writable storage in a dedicated user-data directory (fm4recomp --user-data, required;
// a compatibility policy). The extracted game stays read-only and the raw hard disk (\Device\Harddisk0) stays absent.
// One content device, after Xenia's model (xenia-canary 67f16c4e315f kernel/xam/xam_content_device.{h,cc}): id 1,
// type HDD (1). Its capacity is Xenia's 20 GB, and free space is bounded by the host's (kContentCapacity,
// ContentFree). Content is what is actually stored: nothing at first, then each created item,
// persistent across runs. Layout: <user-data>/content/<device>/<type, 8 hex>/<file name>/ (the item's files) and
// <file name>.xcontent (its 0x134-byte XCONTENT_DATA as created; Xenia content_manager.h:39); an installed package is
// the file <user-data>/content/1/<type>/<file name> itself (FM4 Recomp, Disc 2; below).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime {

constexpr uint32_t kContentDeviceId = 1;            // Xenia DummyDeviceId::HDD
constexpr uint32_t kContentDeviceType = 1;          // Xenia DeviceType::HDD
// FM4 Recomp (Disc 2): the disc drive's content, the packages under the disc's Content\0000000000000000
// (file_system.h DiscContent), as Xenia's second dummy device: id 2 (xam_content_device.h:26 DummyDeviceId::ODD). Only
// enumeration (device 0 or 2) and XamContentResolve answer for it; its device data and state are not modelled (stop).
constexpr uint32_t kDiscContentDeviceId = 2;
// FM4 Recomp (Disc 2): the device reports
// Xenia's dummy hard disk, 20 GB (xenia-canary 67f16c4e315f kernel/xam/xam_content_device.cc:33-40, 20ull * ONE_GB with
// ONE_GB = 1024^3; also the first retail Xbox 360 hard disk's size), and free space is ContentFree: never more than the
// capacity minus what is stored, nor than the host volume's available space minus a reserve kept for the player's Mac
// (policy: 2 GiB). Xenia's own free figure (3 GB, "so it looks a little used") is not used: it is invented.
constexpr uint64_t kContentCapacity = uint64_t(20) << 30;
constexpr uint64_t kHostReserve = uint64_t(2) << 30;
constexpr uint32_t kContentDataSize = 0x134;        // XCONTENT_DATA: device +0, type +4, display name[128] +8, file name[42] +0x108
constexpr uint32_t kContentTypeSavedGame = 1;       // Xenia xbox.h:356
constexpr uint32_t kContentTypeMarketplace = 2;     // Xenia xbox.h:357 (installed downloadable content: enumerated, never created here)

using ContentData = std::array<uint8_t, kContentDataSize>;

// Mounts the user-data directory: it must exist, be a directory and not be a symlink; stops (9) otherwise.
void MountUserData(const std::string& root);
const std::string& UserDataRoot();
// The stored items of a content type on the content device, in file-name byte order: the records of created items and
// the complete installed packages (an incomplete one is logged and left out). A malformed record stops (9).
std::vector<ContentData> ListContent(uint32_t contentType);
uint64_t ContentBytesUsed();
// min(capacity - used, host available - reserve), 0 when either is spent (pure: tested on hand values).
uint64_t ContentFree(uint64_t used, uint64_t hostAvailable);
// ContentFree of what is stored and the user-data volume's available bytes (statvfs f_bavail * f_frsize). Writes that
// would grow stored content past it fail with STATUS_DISK_FULL (imports_io.cpp), so a full host disk refuses cleanly.
uint64_t ContentFreeBytes();
// FM4 Recomp (Disc 2): installed packages. FM4's own installer copies a Disc 2 package byte for byte (a raw STFS file)
// to <user-data>/content/1/<type>/<file name>, through the writable root InstallRoot mounts. Such a file counts as stored
// content only when complete (stfs.h Package::Complete: a copy cut short is shorter), and is then listed by ListContent
// with a record built from its header (device 1, its type, the header's display name, its file name).
std::string InstallRoot(uint32_t contentType);  // the root's name; the type directory is created and mounted once
std::string InstalledPackagePath(const ContentData& data);
bool PackageStored(const ContentData& data);
// XamContentCreateEx(OPEN_EXISTING) of a stored package: Package::Open (its hash tables verified) mounted read-only as
// "<root>:" (file_system.h MountContentRoot). ALREADY_EXISTS (0xB7) if the root name is open, 0 otherwise; the package
// must be stored (PackageStored, else a stop (7)).
uint32_t OpenPackageContent(const ContentData& data, const std::string& root);
// The item's file name (XCONTENT_DATA +0x108, at most 42 characters, NUL-terminated); stops (7) unless it is a safe
// single path component: printable ASCII, no '/', '\\' or ':', not "." or "..", not empty (it names a host directory).
std::string ContentFileName(const ContentData& data);
// Whether the item (its type at +4, its file name) is stored: its directory exists (Xenia content_manager.cc:141
// ContentExists: the package path exists).
bool ContentExists(const ContentData& data);
// Xenia content_manager.cc:215-239 CreateContent: a root name already open, or a stored item, ALREADY_EXISTS (0xB7);
// otherwise the item's directory is created (a failure: ACCESS_DENIED, 5), its record written beside it as passed (a
// failed write stops (9)), and the directory mounted as the writable root "<root>:" (file_system.h MountDirectoryRoot).
// Returns the Win32 result. The root name must be a safe single path component (7 otherwise).
uint32_t CreateContent(const ContentData& data, const std::string& root);
// Xenia content_manager.cc:242-265 OpenContent: a root name already open ALREADY_EXISTS (0xB7); an item not stored
// FILE_NOT_FOUND (2); otherwise the stored item's directory is mounted as the writable root "<root>:". Returns the Win32
// result. The root name must be a safe single path component (7 otherwise).
uint32_t OpenContent(const ContentData& data, const std::string& root);
// Xenia content_manager.cc:318-332 DeleteContent: an item that is open (a root mounted on it) ACCESS_DENIED (5); one not
// stored FILE_NOT_FOUND (2); otherwise its directory and everything in it, then its record, are removed (0). Only
// regular files and directories are removed; a symlink, or a host failure, stops (9).
uint32_t DeleteContent(const ContentData& data);
// XAM app (0xFE) message 0x2000E, XamAppEnumerateContentAggregate (Xenia apps/xam_app.cc:29-71): the next item of an
// aggregate content enumerator into the message's buffer (imports_content.cpp). Returns the HRESULT.
uint32_t EnumerateContentAggregateMessage(uint8_t* base, uint32_t data);

}  // namespace runtime
