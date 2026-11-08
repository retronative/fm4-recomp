// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Saved content: isolated, writable storage in a dedicated user-data directory (fm4recomp --user-data, required;
// a compatibility policy). The extracted game stays read-only and the raw hard disk (\Device\Harddisk0) stays absent.
// One content device, after Xenia's model (xenia-canary 67f16c4e315f kernel/xam/xam_content_device.{h,cc}): id 1,
// type HDD (1). Its capacity is a quota this storage enforces (not Xenia's invented 20 GB / 3 GB free); free space is
// the quota minus what is stored. Content is what is actually stored: nothing at first, then each created item,
// persistent across runs. Layout: <user-data>/content/<device>/<type, 8 hex>/<file name>/ (the item's files) and
// <file name>.xcontent (its 0x134-byte XCONTENT_DATA as created; Xenia content_manager.h:39).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime {

constexpr uint32_t kContentDeviceId = 1;            // Xenia DummyDeviceId::HDD
constexpr uint32_t kContentDeviceType = 1;          // Xenia DeviceType::HDD
constexpr uint64_t kContentQuota = uint64_t(512) << 20;  // policy: 512 MB, enforced
constexpr uint32_t kContentDataSize = 0x134;        // XCONTENT_DATA: device +0, type +4, display name[128] +8, file name[42] +0x108
constexpr uint32_t kContentTypeSavedGame = 1;       // Xenia xbox.h:356
constexpr uint32_t kContentTypeMarketplace = 2;     // Xenia xbox.h:357 (installed downloadable content: enumerated, never created here)

using ContentData = std::array<uint8_t, kContentDataSize>;

// Mounts the user-data directory: it must exist, be a directory and not be a symlink; stops (9) otherwise.
void MountUserData(const std::string& root);
const std::string& UserDataRoot();
// The stored items of a content type on the content device, in file-name byte order. A malformed record stops (9).
std::vector<ContentData> ListContent(uint32_t contentType);
uint64_t ContentBytesUsed();
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
