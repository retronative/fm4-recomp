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
// stops (9) unless the directory and the map are readable and well formed. MountDisc registers it as Disc 1 (the
// launch disc, --disc) and puts it in the drive.
void MountDisc(const std::string& root, const std::string& map);

// FM4 Recomp (Disc 2): the title's other discs (FM4's execution info: disc 1 of 2, system flag
// MULTIDISC_SWAP 0x8000, Xenia xex2_info.h:140). AddDisc registers disc `number` (FM4: Disc 2, --disc2, --disc2-map),
// checked as MountDisc checks Disc 1, without putting it in the drive. InsertDisc puts a registered disc in the drive
// (XamSwapDisc): from then on \Device\Cdrom0 (and game:\, d:\) serve that disc's tree and map. Handles and package
// roots opened on a disc stay tied to it (DiscEntry::disc): reading one while another disc is in the drive stops (7)
// (the console's behaviour for a removed disc is not modelled). InsertDisc of an unregistered disc stops (9).
void AddDisc(uint32_t number, const std::string& root, const std::string& map);
bool DiscRegistered(uint32_t number);
uint32_t DiscInDrive();  // 0 before MountDisc
void InsertDisc(uint32_t number);
// The host path of a registered disc's root file `name` (matched in any case against its map), "" if the map has none.
std::string DiscRootFile(uint32_t number, std::string_view name);
size_t OpenFilesOnDisc(uint32_t number);  // open handles to entries of that disc (the swap's log line)
// The content packages of the disc in the drive, in its GDFX tree order: the files
// Content\0000000000000000\<title>\<type>\<name> (any case) with an 8-character title directory (hex, else a stop (7)),
// the type as 8 hex digits and a 16-character name (Xenia xam_content_aggregate.cc:26-77 AddODDContentTest: rules "????????" and "????????????????").
struct DiscContentItem { uint32_t titleId; std::string fileName; };
std::vector<DiscContentItem> DiscContent(uint32_t contentType);

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
    uint32_t disc = 0;      // the disc it is on (a disc entry, or a file of a package mounted from a disc); 0 elsewhere
    // FM4 Recomp (writable entries): the host file's creation, last access, last write and change times as FILETIMEs (100 ns since
    // 1601-01-01); 0 elsewhere (the disc's fixed stamp is reported instead: DiscTimes).
    uint64_t times[4] = {0, 0, 0, 0};
};
uint32_t DiscAttributes(const DiscEntry& entry);
// The four times a query reports for `entry` (creation, last access, last write, change): a writable entry's host times (Xenia
// 67f16c4 host_path_entry.cc: a host file's own times), else kDiscTimeStamp for each.
void DiscTimes(const DiscEntry& entry, uint64_t (&out)[4]);
// A host time (seconds and nanoseconds since 1970-01-01) as a FILETIME: (seconds * 10^7 + nanoseconds / 100) + 116444736000000000.
uint64_t FileTimeFromUnix(int64_t seconds, int64_t nanoseconds);
uint64_t DiscAllocationSize(const DiscEntry& entry);

// No title update is installed: the translation is of the base default.xex, so update content (an .xexp
// patch, media) would belong to another executable version. Without one, Xenia registers no "UPDATE:"
// link (kernel_state.cc:482-510 ApplyTitleUpdate), so update: paths resolve to nothing and file calls
// return NO_SUCH_FILE. ("update:" in any case, as links are matched.)
bool TitleUpdatePath(std::string_view device);

// Content roots (XamContentOpenFile): a read-only STFS package mounted as "<root>:" (Xenia mounts content as a device
// with a symbolic link "<root>:"; xam_content.cc). Paths "<root>:\..." resolve inside it, in any case; its root is a
// directory, its directories 0x10 and files 0x81 (Xenia stfs_container_device.cc:122-124: directory; normal |
// read-only). Mounting a root that exists, or unmounting one that does not, returns false. `disc`: the disc the
// package file is on (0: not on a disc); resolving inside the root while another disc is in the drive stops (7).
bool MountContentRoot(const std::string& root, std::shared_ptr<const stfs::Package> package, uint32_t disc = 0);
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
// FM4 Recomp (user data): a guest name as stored on the host and back. ':' (FM4's default photo name is the date and time,
// "2026-11-14 17:51") is not valid on every host volume (exFAT), so it is stored as "%3A", and '%' as "%25"; GuestNameOf reverses
// exactly those two (any other '%' sequence is left as it is). Pure.
std::string HostNameOf(std::string_view guest);
std::string GuestNameOf(std::string_view host);
// FM4 Recomp (user data): a host directory entry that is not the game's: "." and "..", and the files macOS adds itself: an
// AppleDouble companion "._<name>" (extended attributes on a volume without them, e.g. exFAT: TN1188 / Apple's AppleDouble
// format) and ".DS_Store" (Finder). Listings of saved content leave them out. Pure.
bool HostOnlyName(std::string_view host);
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
void RemoveHostFile(const DiscEntry& entry);  // FM4 Recomp: an empty saved-content directory too (one still holding files is left)
// Whether a writable root is mounted on this host directory (an open content item).
bool DirectoryRootMountedAt(const std::string& hostDirectory);
// A writable entry's current size on the host (other handles may have written since it was opened).
uint64_t HostFileSize(const DiscEntry& entry);
// fsync of a writable entry's host file (a failure stops (9)).
void SyncHostFile(const DiscEntry& entry);
// Reads up to `size` bytes at `offset` into `buffer`; returns the bytes read (0 at or past the end).
// A failed host read stops (9): the extraction is damaged. An entry of a disc that is not in the drive stops (7).
uint32_t ReadDiscFile(const DiscEntry& entry, uint64_t offset, uint8_t* buffer, uint32_t size);
OpenFile* FindOpenFile(uint32_t body);                          // by the object body a handle names

}  // namespace runtime
