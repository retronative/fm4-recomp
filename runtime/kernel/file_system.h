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
#include <vector>

namespace runtime {

// The host directory holding the extracted disc and the disc map (path TAB sector TAB size TAB attributes, hex);
// stops (9) unless the directory and the map are readable and well formed.
void MountDisc(const std::string& root, const std::string& map);

std::string CanonicalGuestPath(std::string_view path);
// Xenia xboxkrnl_io.cc:42 IsValidPath: printable ASCII without " + , ; < = > |, and without * ? unless `pattern`
// (NtQueryDirectoryFile's file name), where a '*' may only be followed by '.' or end the name (Xenia's comment: a
// title loops forever on "*_X3.rkv" otherwise).
bool ValidGuestPath(std::string_view path, bool pattern = false);
// Xenia base/filesystem_wildcard.cc (WildcardEngine::PreparePattern + Match, copied beside the Xenia sources in
// research/): the name matches the pattern in any ASCII case; an empty pattern matches every name.
bool WildcardMatch(std::string_view pattern, std::string_view name);

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
    std::string relative;  // a disc entry: its path under the disc root ('/'-separated, disc case; "" for the root)
    bool writable = false;  // in a writable content root (saved content, a host directory under the user data)
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
// A writable content root (XamContentCreateEx; Xenia content_manager.cc:35-49 mounts the item's directory as a writable
// HostPathDevice linked as "<root>:"): paths resolve inside `hostDirectory` in any case; a symlink stops (9); directories
// 0x10, files 0x80 (Xenia host_path_entry.cc:40-43). A root name already mounted (either kind) returns false.
bool MountDirectoryRoot(const std::string& root, const std::string& hostDirectory);
bool ContentRootMounted(const std::string& root);
// Makes a root's content durable (XamContentFlush): for a writable root, every file and directory under its host
// directory is fsynced (a symlink stops (9)); a read-only package root has nothing to write. False if no root has that name.
bool FlushContentRoot(const std::string& root);
// Unmounts a root of either kind; false if none has that name.
bool UnmountContentRoot(const std::string& root);

// A directory's children in Xenia's order: on the disc the GDFX tree in order (left, node, right;
// vfs/devices/disc_image_device.cc:126-187), as the disc map lists them; in a package its file table order
// (stfs_container_device.cc:94). A file has none.
struct DirectoryChild { std::string name; DiscEntry entry; };
std::vector<DirectoryChild> ListDirectory(const DiscEntry& directory);

enum class Resolved { Found, NotFound, OtherDevice };
// Resolves a guest path; `device` receives the path after canonicalization and links (for a stop line).
Resolved ResolveGuestPath(std::string_view path, DiscEntry& entry, std::string& device);

// An open disc file: a File object whose guest body is a kernel arena block; the host state is kept
// here by that body. Reads open the host file read-only each time (no descriptor outlives a call).
// The find state (NtQueryDirectoryFile, Xenia kernel/xfile.cc:43 QueryDirectory): the pattern and the index of the next
// child to test; the children are listed at the first query.
struct OpenFile
{
    DiscEntry entry;
    uint64_t position = 0;
    bool synchronous = false;
    std::string findPattern;
    size_t findIndex = 0;
    bool listed = false;
    std::vector<DirectoryChild> children;
    bool deleteOnClose = false;  // FileDispositionInformation: the host file is removed when this handle closes
};
uint32_t OpenDiscFile(uint8_t* base, const DiscEntry& entry, bool synchronous);  // a new handle
// Writable roots only: an empty file (refusing an existing name or a symlink) or a directory; truncation of a file.
bool CreateHostEntry(const std::string& host, bool directory);
bool TruncateHostFile(const std::string& host);
// Writes `size` bytes at `offset` into a writable entry's host file (a failed host write stops (9)); returns `size`.
uint32_t WriteHostFile(const DiscEntry& entry, uint64_t offset, const uint8_t* buffer, uint32_t size);
// Sets a writable host file's length (ftruncate: cut, or extended with zeros); stops (7) for a file outside a writable
// root and (9) if the host refuses.
void SetHostFileLength(const DiscEntry& entry, uint64_t length);
// Removes a writable root's host file (not a directory; a symlink or a host failure stops (9)).
void RemoveHostFile(const DiscEntry& entry);
// Whether a writable root is mounted on this host directory (an open content item).
bool DirectoryRootMountedAt(const std::string& hostDirectory);
// A writable entry's current size on the host (other handles may have written since it was opened).
uint64_t HostFileSize(const DiscEntry& entry);
// fsync of a writable entry's host file (a failure stops (9)).
void SyncHostFile(const DiscEntry& entry);
// Reads up to `size` bytes at `offset` into `buffer`; returns the bytes read (0 at or past the end).
// A failed host read stops (9): the extraction is damaged.
uint32_t ReadDiscFile(const DiscEntry& entry, uint64_t offset, uint8_t* buffer, uint32_t size);
OpenFile* FindOpenFile(uint32_t body);                          // by the object body a handle names

}  // namespace runtime
