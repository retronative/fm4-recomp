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

using ContentData = std::array<uint8_t, kContentDataSize>;

// Mounts the user-data directory: it must exist, be a directory and not be a symlink; stops (9) otherwise.
void MountUserData(const std::string& root);
const std::string& UserDataRoot();
// The stored items of a content type on the content device, in file-name byte order. A malformed record stops (9).
std::vector<ContentData> ListContent(uint32_t contentType);
uint64_t ContentBytesUsed();

}  // namespace runtime
