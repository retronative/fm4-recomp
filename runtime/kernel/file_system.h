// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The title's disc, served read-only from the player's extracted Disc 1 (--disc, game/files). Guest
// paths follow Xenia's virtual file system (xenia-canary 67f16c4e315f):
//   1. canonicalize: split on '\' and '/', drop empty parts and ".", ".." removes the previous part
//      unless it is a device ("x:"); rooted if it starts with '\' (base/utf8.cc:684 canonicalize_path);
//   2. symbolic links "game:" and "d:" -> "\Device\Cdrom0", prefix matched in any case
//      (emulator.cc:405-406, vfs/virtual_file_system.cc:87 ResolveSymbolicLink);
//   3. the device by its mount path, prefix matched exactly (virtual_file_system.cc:108);
//   4. each remaining part matched in any case against a directory's children (vfs/entry.cc:49-72).
// Entries carry the disc device's values (vfs/devices/disc_image_device.cc:146-153): all time stamps
// 1970-01-01 (FILETIME 116444736000000000), allocation = size rounded up to the 2048-byte sector, the
// read-only attribute added. Sectors, sizes and attribute bytes come from the disc's own GDFX directory
// (survey/gdfx.py writes game/survey/disc1_gdfx.tsv from the owner's image; the extraction keeps none of them).
// OBSERVED: files 0x80, directories 0x90 (reported 0x81 and 0x91); the root is 0x10 (Xenia root_entry).
#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <string_view>

namespace runtime {

// The host directory holding the extracted disc and the disc map (path TAB sector TAB size TAB attributes, hex);
// stops (9) unless the directory and the map are readable and well formed.
void MountDisc(const std::string& root, const std::string& map);

std::string CanonicalGuestPath(std::string_view path);
// Xenia xboxkrnl_io.cc:42 IsValidPath (not a pattern): printable ASCII without " * + , ; < = > ? |.
bool ValidGuestPath(std::string_view path);

constexpr uint64_t kDiscTimeStamp = 116444736000000000ull;  // 1970-01-01 as FILETIME
constexpr uint32_t kDiscSectorSize = 2048;
namespace file_attribute { constexpr uint32_t kReadOnly = 0x01, kDirectory = 0x10; }

namespace stfs { class Package; }
// A file or directory of the disc, or of a mounted content package (`package` set: `packageFile` indexes its files,
// -1 for its root; reads go through the package).
struct DiscEntry
{
    std::string host;
    uint64_t size = 0;
    bool directory = false;
    uint32_t sector = 0, attributes = 0;
    std::shared_ptr<const stfs::Package> package;
    int32_t packageFile = -1;
};
uint32_t DiscAttributes(const DiscEntry& entry);
uint64_t DiscAllocationSize(const DiscEntry& entry);

// No title update is installed: the translation is of the base default.xex, so update content (an .xexp
// patch, media) would belong to another executable version. Without one, Xenia registers no "UPDATE:"
// link (kernel_state.cc:482-510 ApplyTitleUpdate), so update: paths resolve to nothing and file calls
// return NO_SUCH_FILE. ("update:" in any case, as links are matched.)
bool TitleUpdatePath(std::string_view device);

// Content roots (XamContentOpenFile): a read-only STFS package mounted as "<root>:" (Xenia mounts content as a device
// with a symbolic link "<root>:"; xam_content.cc). Paths "<root>:\..." resolve inside it, in any case; its root is a
// directory, its directories 0x10 and files 0x81 (Xenia stfs_container_device.cc:122-124: directory; normal |
// read-only). Mounting a root that exists, or unmounting one that does not, returns false.
bool MountContentRoot(const std::string& root, std::shared_ptr<const stfs::Package> package);
bool UnmountContentRoot(const std::string& root);

enum class Resolved { Found, NotFound, OtherDevice };
// Resolves a guest path; `device` receives the path after canonicalization and links (for a stop line).
Resolved ResolveGuestPath(std::string_view path, DiscEntry& entry, std::string& device);

// An open disc file: a File object whose guest body is a kernel arena block; the host state is kept
// here by that body. Reads open the host file read-only each time (no descriptor outlives a call).
struct OpenFile { DiscEntry entry; uint64_t position = 0; bool synchronous = false; };
uint32_t OpenDiscFile(uint8_t* base, const DiscEntry& entry, bool synchronous);  // a new handle
// Reads up to `size` bytes at `offset` into `buffer`; returns the bytes read (0 at or past the end).
// A failed host read stops (9): the extraction is damaged.
uint32_t ReadDiscFile(const DiscEntry& entry, uint64_t offset, uint8_t* buffer, uint32_t size);
OpenFile* FindOpenFile(uint32_t body);                          // by the object body a handle names

}  // namespace runtime
