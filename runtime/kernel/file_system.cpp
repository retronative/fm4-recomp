// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The title's disc, read-only (file_system.h).
#include "file_system.h"
#include "arena.h"
#include "dispatcher.h"
#include "objects.h"
#include "stops.h"
#include "stfs.h"
#include "virtual_memory.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>

namespace runtime {
namespace {

struct MapEntry { uint32_t sector; uint64_t size; uint32_t attributes; size_t rank; };  // rank: the line (GDFX tree order)
// One disc of the set: the realpath of its extraction and its map (by path relative to the root, '/'-separated, disc case).
struct Disc { uint32_t number; std::string root; std::unordered_map<std::string, MapEntry> map; };
std::mutex discLock;  // guards the set and the drive: XamSwapDisc changes the drive while other threads resolve paths
std::map<uint32_t, std::shared_ptr<const Disc>> discs;
std::shared_ptr<const Disc> drive;  // the disc in the drive (\Device\Cdrom0)
constexpr std::string_view kCdrom = "\\Device\\Cdrom0";
constexpr std::string_view kLinks[] = {"game:", "d:"};

char Lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
bool EqualCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (Lower(a[i]) != Lower(b[i]))
            return false;
    return true;
}

std::vector<std::string_view> SplitPath(std::string_view path)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); i++)
        if (i == path.size() || path[i] == '\\' || path[i] == '/')
        {
            if (i > start)
                parts.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    return parts;
}

// The disc in the drive, or a stop (9) before MountDisc.
std::shared_ptr<const Disc> DriveDisc()
{
    std::lock_guard guard(discLock);
    if (!drive)
        Stop(StopCode::Setup, "no disc is mounted (--disc)");
    return drive;
}

// Reads and checks a disc's extraction and map; the options named in its stops are Disc 1's --disc/--disc-map, else
// --disc<n>/--disc<n>-map.
std::shared_ptr<const Disc> LoadDisc(uint32_t number, const std::string& root, const std::string& map)
{
    const std::string option = number == 1 ? "--disc" : "--disc" + std::to_string(number);
    auto disc = std::make_shared<Disc>();
    disc->number = number;
    std::ifstream in(map);
    if (!in)
        Stop(StopCode::Setup, "%s-map %s cannot be read (survey/gdfx.py writes it)", option.c_str(), map.c_str());
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string path, sector, size, attributes;
        if (!std::getline(fields, path, '\t') || !std::getline(fields, sector, '\t') || !std::getline(fields, size, '\t') ||
            !std::getline(fields, attributes) || path.empty())
            Stop(StopCode::Setup, "%s-map %s: a malformed line: %.80s", option.c_str(), map.c_str(), line.c_str());
        const size_t rank = disc->map.size();
        if (!disc->map.emplace(path, MapEntry{uint32_t(std::stoul(sector, nullptr, 16)), std::stoull(size, nullptr, 16),
                                              uint32_t(std::stoul(attributes, nullptr, 16)), rank}).second)
            Stop(StopCode::Setup, "%s-map %s lists %s twice", option.c_str(), map.c_str(), path.c_str());
    }
    if (disc->map.empty())
        Stop(StopCode::Setup, "%s-map %s is empty", option.c_str(), map.c_str());
    char resolved[PATH_MAX];
    struct stat info;
    if (realpath(root.c_str(), resolved) == nullptr || stat(resolved, &info) != 0 || !S_ISDIR(info.st_mode))
        Stop(StopCode::Setup, "%s %s is not a directory (the extracted Disc %u%s)", option.c_str(), root.c_str(), number,
             number == 1 ? ", game/files" : "");
    disc->root = resolved;
    return disc;
}

}  // namespace

void MountDisc(const std::string& root, const std::string& map)
{
    auto disc = LoadDisc(1, root, map);
    std::lock_guard guard(discLock);
    discs[1] = disc;
    drive = disc;
}

void AddDisc(uint32_t number, const std::string& root, const std::string& map)
{
    if (number < 2)
        Stop(StopCode::Setup, "AddDisc(%u): Disc 1 is the launch disc (MountDisc)", number);
    auto disc = LoadDisc(number, root, map);
    std::lock_guard guard(discLock);
    discs[number] = disc;
}

bool DiscRegistered(uint32_t number)
{
    std::lock_guard guard(discLock);
    return discs.count(number) == 1;
}

uint32_t DiscInDrive()
{
    std::lock_guard guard(discLock);
    return drive ? drive->number : 0;
}

void InsertDisc(uint32_t number)
{
    std::lock_guard guard(discLock);
    const auto it = discs.find(number);
    if (it == discs.end())
        Stop(StopCode::Setup, "Disc %u is not set up (--disc%u, --disc%u-map)", number, number, number);
    drive = it->second;
}

std::string DiscRootFile(uint32_t number, std::string_view name)
{
    std::shared_ptr<const Disc> disc;
    {
        std::lock_guard guard(discLock);
        const auto it = discs.find(number);
        if (it == discs.end())
            return "";
        disc = it->second;
    }
    for (const auto& [path, m] : disc->map)
        if ((m.attributes & file_attribute::kDirectory) == 0 && EqualCase(path, name))
            return disc->root + "/" + path;
    return "";
}

std::vector<DiscContentItem> DiscContent(uint32_t contentType)
{
    const auto disc = DriveDisc();
    char type[9];
    std::snprintf(type, sizeof type, "%08X", contentType);
    std::vector<std::pair<size_t, DiscContentItem>> ranked;
    for (const auto& [path, m] : disc->map)
    {
        const std::vector<std::string_view> parts = SplitPath(path);
        if (parts.size() != 5 || (m.attributes & file_attribute::kDirectory) != 0 || !EqualCase(parts[0], "Content") ||
            parts[1] != "0000000000000000" || parts[2].size() != 8 || !EqualCase(parts[3], type) || parts[4].size() != 16)
            continue;
        const std::string title(parts[2]);
        if (title.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            Stop(StopCode::Unimplemented, "the disc's content directory %s is not a title id (8 hex digits)", path.c_str());
        ranked.push_back({m.rank, {uint32_t(std::stoul(title, nullptr, 16)), std::string(parts[4])}});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<DiscContentItem> items;
    for (auto& r : ranked)
        items.push_back(std::move(r.second));
    return items;
}

std::string CanonicalGuestPath(std::string_view path)
{
    std::vector<std::string_view> parts = SplitPath(path);
    for (size_t i = 0; i < parts.size();)
    {
        if (parts[i] == ".")
            parts.erase(parts.begin() + long(i));
        else if (parts[i] == "..")
        {
            if (i > 0 && parts[i - 1].back() != ':')  // never above a device
            {
                parts.erase(parts.begin() + long(i) - 1);
                i--;
            }
            parts.erase(parts.begin() + long(i));
        }
        else
            i++;
    }
    std::string result = !path.empty() && path[0] == '\\' ? "\\" : "";
    for (size_t i = 0; i < parts.size(); i++)
        result += std::string(parts[i]) + (i + 1 < parts.size() ? "\\" : "");
    return result;
}

bool ValidGuestPath(std::string_view path, bool pattern)
{
    bool asterisk = false;
    for (const char c : path)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 31 || u >= 127)
            return false;
        if (asterisk)
        {
            if (c != '.')
                return false;
            asterisk = false;
        }
        switch (c)
        {
        case '"': case '+': case ',': case ';': case '<': case '=': case '>': case '|':
            return false;
        case '*':
            if (!pattern)
                return false;
            asterisk = true;
            break;
        case '?':
            if (!pattern)
                return false;
            break;
        default:
            break;
        }
    }
    return true;
}

bool WildcardMatch(std::string_view pattern, std::string_view name)
{
    // Xenia's rules: literal runs (the first from the start, the last to the end, both when the pattern has no
    // wildcard), each run of '?' a rule matching exactly that many characters, '*' only separating rules.
    struct Rule { std::string match; bool fromStart, toEnd, exactLength; };
    auto lower = [](std::string_view v) {
        std::string out(v);
        for (char& c : out)
            c = c >= 'A' && c <= 'Z' ? char(c + 32) : c;
        return out;
    };
    std::vector<Rule> rules;
    bool first = true;
    size_t n = 0, last = 0;
    while ((n = pattern.find_first_of("*?", last)) != std::string_view::npos)
    {
        if (last != n)
            rules.push_back({lower(pattern.substr(last, n - last)), first, false, false});
        if (pattern[n] == '?')
        {
            const size_t end = pattern.find_first_not_of('?', n + 1);
            const size_t count = end == std::string_view::npos ? pattern.size() - n : end - n;
            rules.push_back({std::string(pattern.substr(n, count)), false, false, true});
            last = n + count;
        }
        else
            last = n + 1;
        first = false;
    }
    if (last != pattern.size())
        rules.push_back({lower(pattern.substr(last)), last == 0, true, false});
    const std::string text = lower(name);
    size_t offset = 0;
    for (const Rule& r : rules)
    {
        if (r.match.empty())
            continue;
        if (text.size() - offset < r.match.size())
            return false;
        if (r.exactLength)
        {
            offset += r.match.size();
            continue;
        }
        const size_t at = text.find(r.match, offset);
        if (at == std::string::npos || (r.fromStart && at != offset) || (r.toEnd && at != text.size() - r.match.size()))
            return false;
        offset = at + r.match.size();
    }
    return true;
}

bool TitleUpdatePath(std::string_view device)
{
    constexpr std::string_view kUpdate = "update:";
    return device.size() >= kUpdate.size() && EqualCase(device.substr(0, kUpdate.size()), kUpdate);
}

uint32_t DiscAttributes(const DiscEntry& entry)
{
    return entry.attributes;
}

uint64_t FileTimeFromUnix(int64_t seconds, int64_t nanoseconds)
{
    return uint64_t(seconds * 10000000 + nanoseconds / 100) + kDiscTimeStamp;  // kDiscTimeStamp is 1970-01-01 as a FILETIME
}

void DiscTimes(const DiscEntry& entry, uint64_t (&out)[4])
{
    for (int i = 0; i < 4; i++)
        out[i] = entry.writable && entry.times[i] != 0 ? entry.times[i] : kDiscTimeStamp;
}

uint64_t DiscAllocationSize(const DiscEntry& entry)
{
    return (entry.size + kDiscSectorSize - 1) / kDiscSectorSize * kDiscSectorSize;
}

namespace {
std::mutex rootsLock;
struct PackageRoot { std::shared_ptr<const stfs::Package> package; uint32_t disc; };  // disc: the package file's (0: none)
std::map<std::string, PackageRoot> contentRoots;                             // by lower-case root name
std::map<std::string, std::string> directoryRoots;                           // by lower-case root name: the host directory

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

DiscEntry PackageEntry(const std::shared_ptr<const stfs::Package>& package, int32_t at, uint32_t disc)
{
    DiscEntry entry;
    entry.host = package->Host();
    entry.disc = disc;
    entry.package = package;
    entry.packageFile = at;
    entry.directory = at < 0 || package->Files()[size_t(at)].directory;
    entry.size = entry.directory ? 0 : package->Files()[size_t(at)].length;
    entry.attributes = entry.directory ? file_attribute::kDirectory : 0x80 | file_attribute::kReadOnly;
    return entry;
}

// FM4 Recomp: a writable entry's host times (DiscEntry::times) from its lstat (macOS: st_birthtimespec is the creation time).
void SetHostTimes(DiscEntry& entry, const struct stat& info)
{
    entry.times[0] = FileTimeFromUnix(info.st_birthtimespec.tv_sec, info.st_birthtimespec.tv_nsec);
    entry.times[1] = FileTimeFromUnix(info.st_atimespec.tv_sec, info.st_atimespec.tv_nsec);
    entry.times[2] = FileTimeFromUnix(info.st_mtimespec.tv_sec, info.st_mtimespec.tv_nsec);
    entry.times[3] = FileTimeFromUnix(info.st_ctimespec.tv_sec, info.st_ctimespec.tv_nsec);
}
void SetHostTimes(DiscEntry& entry)
{
    struct stat info;
    if (lstat(entry.host.c_str(), &info) == 0)
        SetHostTimes(entry, info);
}

// A path inside a writable root's host directory: each part matched in any case (names as GuestNameOf gives them); a symlink stops (9).
Resolved ResolveDirectoryPath(const std::string& hostDirectory, std::string_view rest, DiscEntry& entry)
{
    std::string host = hostDirectory;
    bool directory = true;
    uint64_t size = 0;
    const std::vector<std::string_view> parts = SplitPath(rest);
    entry = DiscEntry{};
    entry.writable = true;  // NotFound: `host` names where the last part would be created ("" if its parent is missing)
    for (size_t k = 0; k < parts.size(); k++)
    {
        const std::string_view part = parts[k];
        if (!directory)
            return Resolved::NotFound;
        std::string match;
        int matches = 0;
        DIR* listing = opendir(host.c_str());
        if (listing == nullptr)
            Stop(StopCode::Setup, "user data: cannot list %s", host.c_str());
        while (const dirent* d = readdir(listing))
            if (!HostOnlyName(d->d_name) && EqualCase(GuestNameOf(d->d_name), part))
                match = d->d_name, matches++;
        closedir(listing);
        if (matches == 0)
        {
            if (k + 1 == parts.size())
                entry.host = host + "/" + HostNameOf(part);
            return Resolved::NotFound;
        }
        if (matches > 1)
            Stop(StopCode::Setup, "user data: %s holds %d names equal to %.*s in any case", host.c_str(), matches, int(part.size()), part.data());
        host += "/" + match;
        struct stat info;
        if (lstat(host.c_str(), &info) != 0 || S_ISLNK(info.st_mode) || !(S_ISDIR(info.st_mode) || S_ISREG(info.st_mode)))
            Stop(StopCode::Setup, "user data: %s is a symlink, unreadable or not a file or directory", host.c_str());
        directory = S_ISDIR(info.st_mode);
        size = directory ? 0 : uint64_t(info.st_size);
    }
    entry = DiscEntry{};
    entry.host = host;
    entry.directory = directory;
    entry.size = size;
    entry.attributes = directory ? file_attribute::kDirectory : 0x80;
    entry.writable = true;
    SetHostTimes(entry);
    return Resolved::Found;
}

// "<root>:\a\b" inside a mounted package or writable root; any other device is OtherDevice.
Resolved ResolveContentPath(const std::string& device, DiscEntry& entry)
{
    const size_t colon = device.find(':');
    if (colon == std::string::npos)
        return Resolved::OtherDevice;
    std::shared_ptr<const stfs::Package> package;
    uint32_t packageDisc = 0;
    {
        std::lock_guard guard(rootsLock);
        if (const auto d = directoryRoots.find(Lower(std::string_view(device).substr(0, colon))); d != directoryRoots.end())
        {
            const std::string hostDirectory = d->second;
            return ResolveDirectoryPath(hostDirectory, std::string_view(device).substr(colon + 1), entry);
        }
        const auto it = contentRoots.find(Lower(std::string_view(device).substr(0, colon)));
        if (it == contentRoots.end())
            return Resolved::OtherDevice;
        package = it->second.package, packageDisc = it->second.disc;
    }
    if (packageDisc != 0 && packageDisc != DiscInDrive())
        Stop(StopCode::Unimplemented, "%s: the root is a package on Disc %u, and Disc %u is in the drive (not modelled)", device.c_str(),
             packageDisc, DiscInDrive());
    int32_t at = -1;  // the package root
    for (const std::string_view part : SplitPath(std::string_view(device).substr(colon + 1)))
    {
        if (at >= 0 && !package->Files()[size_t(at)].directory)
            return Resolved::NotFound;
        int32_t found = -1;
        for (size_t i = 0; i < package->Files().size(); i++)
            if (package->Files()[i].parent == at && EqualCase(package->Files()[i].name, part))
                found = int32_t(i);
        if (found < 0)
            return Resolved::NotFound;
        at = found;
    }
    entry = PackageEntry(package, at, packageDisc);
    return Resolved::Found;
}
}  // namespace

bool MountContentRoot(const std::string& root, std::shared_ptr<const stfs::Package> package, uint32_t disc)
{
    std::lock_guard guard(rootsLock);
    return directoryRoots.count(Lower(root)) == 0 && contentRoots.emplace(Lower(root), PackageRoot{std::move(package), disc}).second;
}

bool MountDirectoryRoot(const std::string& root, const std::string& hostDirectory)
{
    std::lock_guard guard(rootsLock);
    return contentRoots.count(Lower(root)) == 0 && directoryRoots.emplace(Lower(root), hostDirectory).second;
}

bool ContentRootMounted(const std::string& root)
{
    std::lock_guard guard(rootsLock);
    return contentRoots.count(Lower(root)) == 1 || directoryRoots.count(Lower(root)) == 1;
}

namespace {
void SyncTree(const std::string& path)
{
    struct stat info;
    if (lstat(path.c_str(), &info) != 0 || S_ISLNK(info.st_mode))
        Stop(StopCode::Setup, "user data: %s is gone or a symbolic link", path.c_str());
    if (S_ISDIR(info.st_mode))
        if (DIR* dir = opendir(path.c_str()))
        {
            while (const dirent* e = readdir(dir))
                if (std::string(e->d_name) != "." && std::string(e->d_name) != "..")
                    SyncTree(path + "/" + e->d_name);
            closedir(dir);
        }
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fsync(fd) != 0 || close(fd) != 0)
        Stop(StopCode::Setup, "user data: flushing %s failed", path.c_str());
}
}  // namespace

bool FlushContentRoot(const std::string& root)
{
    std::string host;
    {
        std::lock_guard guard(rootsLock);
        if (contentRoots.count(Lower(root)))
            return true;
        const auto found = directoryRoots.find(Lower(root));
        if (found == directoryRoots.end())
            return false;
        host = found->second;
    }
    SyncTree(host);
    return true;
}

bool UnmountContentRoot(const std::string& root)
{
    std::lock_guard guard(rootsLock);
    return contentRoots.erase(Lower(root)) + directoryRoots.erase(Lower(root)) == 1;
}

Resolved ResolveGuestPath(std::string_view path, DiscEntry& entry, std::string& device)
{
    device = CanonicalGuestPath(path);
    for (bool linked = true; linked;)
    {
        linked = false;
        for (const std::string_view link : kLinks)
            if (device.size() >= link.size() && EqualCase(std::string_view(device).substr(0, link.size()), link))
            {
                device = std::string(kCdrom) + device.substr(link.size());
                linked = true;
            }
    }
    if (device.compare(0, kCdrom.size(), kCdrom) != 0)
        return ResolveContentPath(device, entry);
    const auto disc = DriveDisc();
    entry.disc = disc->number;
    std::string host = disc->root, relative;
    bool directory = true;
    for (const std::string_view part : SplitPath(std::string_view(device).substr(kCdrom.size())))
    {
        if (!directory)
            return Resolved::NotFound;  // a file has no children
        DIR* listing = opendir(host.c_str());
        if (listing == nullptr)
            Stop(StopCode::Setup, "cannot list %s on the disc", host.c_str());
        std::string match;
        int matches = 0;
        while (const dirent* d = readdir(listing))
            if (EqualCase(d->d_name, part) && std::string_view(d->d_name) != "." && std::string_view(d->d_name) != "..")
            {
                match = d->d_name;
                matches++;
            }
        closedir(listing);
        if (matches == 0)
            return Resolved::NotFound;
        if (matches > 1)
            Stop(StopCode::Setup, "%s holds %d names equal to %.*s in any case", host.c_str(), matches, int(part.size()), part.data());
        host += "/" + match;
        relative += (relative.empty() ? "" : "/") + match;
        struct stat info;
        if (lstat(host.c_str(), &info) != 0 || S_ISLNK(info.st_mode))
            Stop(StopCode::Setup, "%s on the disc is a symlink or unreadable", host.c_str());
        directory = S_ISDIR(info.st_mode);
        entry.size = directory ? 0 : uint64_t(info.st_size);
    }
    entry.host = host;
    entry.directory = directory;
    entry.relative = relative;
    if (relative.empty())  // the root (Xenia disc_image_device.cc:116: directory, no other attribute)
    {
        entry.size = 0, entry.sector = 0, entry.attributes = file_attribute::kDirectory;
        return Resolved::Found;
    }
    const auto it = disc->map.find(relative);
    if (it == disc->map.end() || ((it->second.attributes & file_attribute::kDirectory) != 0) != directory ||
        (!directory && it->second.size != entry.size))
        Stop(StopCode::Setup, "%s on the disc does not match the disc map (not listed, or another kind or size)", relative.c_str());
    entry.size = it->second.size;  // a directory: its directory data size (Xenia: the dirent length)
    entry.sector = it->second.sector;
    entry.attributes = it->second.attributes | file_attribute::kReadOnly;  // (:146)
    return Resolved::Found;
}

std::vector<DirectoryChild> ListDirectory(const DiscEntry& directory)
{
    std::vector<DirectoryChild> children;
    if (!directory.directory)
        return children;
    if (directory.writable)
    {
        // A writable root's host directory (saved content, e.g. FM4's photos): its children as ResolveDirectoryPath describes each
        // (Xenia host_path_entry.cc:30-45: a directory, else kFileAttributeNormal), in byte order of their names (Xenia lists the
        // host directory's own order, which macOS does not fix); a symlink or another kind stops (9) as when resolving one.
        DIR* listing = opendir(directory.host.c_str());
        if (listing == nullptr)
            Stop(StopCode::Setup, "user data: cannot list %s", directory.host.c_str());
        std::vector<std::pair<std::string, std::string>> names;  // (guest name, host name), in byte order of the guest names
        while (const dirent* d = readdir(listing))
            if (!HostOnlyName(d->d_name))
                names.emplace_back(GuestNameOf(d->d_name), d->d_name);
        closedir(listing);
        std::sort(names.begin(), names.end());
        for (const auto& [name, hostName] : names)
        {
            DiscEntry e;
            e.writable = true;
            e.host = directory.host + "/" + hostName;
            struct stat info;
            if (lstat(e.host.c_str(), &info) != 0 || S_ISLNK(info.st_mode) || !(S_ISDIR(info.st_mode) || S_ISREG(info.st_mode)))
                Stop(StopCode::Setup, "user data: %s is a symlink, unreadable or not a file or directory", e.host.c_str());
            e.directory = S_ISDIR(info.st_mode);
            e.size = e.directory ? 0 : uint64_t(info.st_size);
            e.attributes = e.directory ? file_attribute::kDirectory : 0x80;
            SetHostTimes(e, info);
            children.push_back({name, e});
        }
        return children;
    }
    if (directory.package)
    {
        const auto& files = directory.package->Files();
        for (size_t i = 0; i < files.size(); i++)
            if (files[i].parent == directory.packageFile)
                children.push_back({files[i].name, PackageEntry(directory.package, int32_t(i), directory.disc)});
        return children;
    }
    const auto disc = DriveDisc();
    if (directory.disc != disc->number)
        Stop(StopCode::Unimplemented, "listing \\%s of Disc %u while Disc %u is in the drive is not modelled", directory.relative.c_str(),
             directory.disc, disc->number);
    const std::string prefix = directory.relative.empty() ? "" : directory.relative + "/";
    std::vector<std::pair<size_t, DirectoryChild>> ranked;
    for (const auto& [path, m] : disc->map)
    {
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0 || path.find('/', prefix.size()) != std::string::npos)
            continue;
        DiscEntry e;
        e.host = disc->root + "/" + path;
        e.relative = path;
        e.disc = disc->number;
        e.directory = (m.attributes & file_attribute::kDirectory) != 0;
        e.size = m.size;
        e.sector = m.sector;
        e.attributes = m.attributes | file_attribute::kReadOnly;  // as ResolveGuestPath
        ranked.push_back({m.rank, {path.substr(prefix.size()), e}});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& r : ranked)
        children.push_back(std::move(r.second));
    return children;
}

namespace {
std::mutex filesLock;
std::map<uint32_t, OpenFile> files;  // by object body
}  // namespace

bool CreateHostEntry(const std::string& host, bool directory)
{
    if (directory)
        return mkdir(host.c_str(), 0755) == 0;
    const int fd = open(host.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    return fd >= 0 && close(fd) == 0;
}

bool TruncateHostFile(const std::string& host)
{
    const int fd = open(host.c_str(), O_WRONLY | O_TRUNC | O_NOFOLLOW);
    return fd >= 0 && close(fd) == 0;
}

void RemoveHostFile(const DiscEntry& entry)
{
    struct stat info;
    if (!entry.writable)
        Stop(StopCode::Unimplemented, "removing %s: not in a writable content root", entry.host.c_str());
    if (entry.directory)  // FM4 Recomp: an empty saved-content directory; one still holding files is left as it is (logged)
    {
        if (lstat(entry.host.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
            Stop(StopCode::Setup, "user data: removing %s failed (gone or not a directory)", entry.host.c_str());
        if (rmdir(entry.host.c_str()) != 0)
            std::fprintf(stderr, "[fs] %s: delete-on-close of a directory that still holds files: left in place\n", entry.host.c_str());
        return;
    }
    if (lstat(entry.host.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || unlink(entry.host.c_str()) != 0)
        Stop(StopCode::Setup, "user data: removing %s failed (gone, not a regular file, or refused)", entry.host.c_str());
}

bool DirectoryRootMountedAt(const std::string& hostDirectory)
{
    std::lock_guard guard(rootsLock);
    for (const auto& [root, host] : directoryRoots)
        if (host == hostDirectory)
            return true;
    return false;
}

void SetHostFileLength(const DiscEntry& entry, uint64_t length)
{
    if (!entry.writable || entry.directory)
        Stop(StopCode::Unimplemented, "setting the length of %s: not a file in a writable content root", entry.host.c_str());
    const int fd = open(entry.host.c_str(), O_WRONLY | O_NOFOLLOW);
    if (fd < 0 || ftruncate(fd, off_t(length)) != 0 || close(fd) != 0)
        Stop(StopCode::Setup, "user data: setting the length of %s to %llu failed", entry.host.c_str(), (unsigned long long)length);
}

uint32_t WriteHostFile(const DiscEntry& entry, uint64_t offset, const uint8_t* buffer, uint32_t size)
{
    if (!entry.writable || entry.directory)
        Stop(StopCode::Unimplemented, "writing %s: not a file in a writable content root", entry.host.c_str());
    const int fd = open(entry.host.c_str(), O_WRONLY | O_NOFOLLOW);
    if (fd < 0)
        Stop(StopCode::Setup, "user data: cannot open %s for writing", entry.host.c_str());
    uint32_t done = 0;
    while (done < size)
    {
        const ssize_t n = pwrite(fd, buffer + done, size - done, off_t(offset + done));
        if (n <= 0)
        {
            close(fd);
            Stop(StopCode::Setup, "user data: writing %s at %llu failed", entry.host.c_str(), (unsigned long long)(offset + done));
        }
        done += uint32_t(n);
    }
    if (close(fd) != 0)
        Stop(StopCode::Setup, "user data: closing %s after a write failed", entry.host.c_str());
    return done;
}

void SyncHostFile(const DiscEntry& entry)
{
    const int fd = open(entry.host.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fsync(fd) != 0 || close(fd) != 0)
        Stop(StopCode::Setup, "user data: flushing %s failed", entry.host.c_str());
}

uint64_t HostFileSize(const DiscEntry& entry)
{
    struct stat info;
    if (lstat(entry.host.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        Stop(StopCode::Setup, "user data: %s is gone or not a regular file", entry.host.c_str());
    return uint64_t(info.st_size);
}

uint32_t OpenDiscFile(uint8_t* base, const DiscEntry& entry, bool synchronous)
{
    // The body is a waitable object, as Xenia's XFile (xfile.cc:27-32 at 67f16c4): an auto-reset event, not signalled, that
    // each completed NtReadFile and NtWriteFile sets (FM4's ReadFile without an OVERLAPPED waits on the file handle after
    // PENDING, the game's wait loop; a Disc 2 install waited there forever on a body that was no event).
    const uint32_t body = ArenaAlloc(base, 16);
    std::memset(base + body, 0, 16), base[body] = dispatch::kSynchronizationEvent;  // signal state (+4) 0
    {
        std::lock_guard guard(filesLock);
        OpenFile file;
        file.entry = entry, file.synchronous = synchronous;
        files[body] = std::move(file);
    }
    return CreateObject(ObjectType::File, body, "");
}

OpenFile* FindOpenFile(uint32_t body)
{
    std::lock_guard guard(filesLock);
    const auto it = files.find(body);
    return it == files.end() ? nullptr : &it->second;
}

size_t OpenFilesOnDisc(uint32_t number)
{
    std::lock_guard guard(filesLock);
    size_t count = 0;
    for (const auto& [body, file] : files)
        count += file.entry.disc == number;
    return count;
}

uint32_t ReadDiscFile(const DiscEntry& entry, uint64_t offset, uint8_t* buffer, uint32_t size)
{
    // FM4 Recomp (Disc 2): a handle opened on a disc that has been swapped out. What the console answers (an error, or a
    // read of the new disc) is not known, so this stops; FM4 preloads its swap music (Media/audio/disc2music, 7 MB) from
    // Disc 1, which suggests it reads nothing from Disc 1 while Disc 2 is in (HYPOTHESIS).
    if (entry.disc != 0 && entry.disc != DiscInDrive())
        Stop(StopCode::Unimplemented, "reading %s of Disc %u while Disc %u is in the drive is not modelled", entry.host.c_str(), entry.disc,
             DiscInDrive());
    if (entry.package)
        return entry.packageFile < 0 ? 0 : entry.package->Read(uint32_t(entry.packageFile), offset, buffer, size);
    if (offset >= entry.size)
        return 0;
    const uint32_t wanted = uint32_t(std::min<uint64_t>(size, entry.size - offset));
    const int fd = open(entry.host.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        Stop(StopCode::Setup, "cannot open %s on the disc for reading", entry.host.c_str());
    uint32_t done = 0;
    // FM4 Recomp (FM4_WRITE_WATCH): a system call cannot fault on a watched page, it returns EFAULT; the watched pages of the
    // buffer are made writable first (virtual_memory.h BeforeHostWrite), and a page protected again meanwhile (a texture load on
    // the command processor's thread) is read through a host buffer and copied with ordinary stores, which fault as usual.
    BeforeHostWrite(buffer, wanted);
    while (done < wanted)
    {
        ssize_t n = pread(fd, buffer + done, wanted - done, off_t(offset + done));
        if (n < 0 && errno == EFAULT)
        {
            std::vector<uint8_t> bounce(wanted - done);
            n = pread(fd, bounce.data(), bounce.size(), off_t(offset + done));
            if (n > 0)
                std::memcpy(buffer + done, bounce.data(), size_t(n));
        }
        if (n <= 0)
        {
            close(fd);
            Stop(StopCode::Setup, "reading %s at %llu failed (the extraction is shorter than its size?)", entry.host.c_str(),
                 (unsigned long long)(offset + done));
        }
        done += uint32_t(n);
    }
    close(fd);
    return done;
}

std::string HostNameOf(std::string_view guest)
{
    std::string out;
    for (const char c : guest)
        out += c == '%' ? std::string("%25") : c == ':' ? std::string("%3A") : std::string(1, c);
    return out;
}

bool HostOnlyName(std::string_view host)
{
    return host == "." || host == ".." || host.substr(0, 2) == "._" || host == ".DS_Store";
}

std::string GuestNameOf(std::string_view host)
{
    std::string out;
    for (size_t i = 0; i < host.size(); i++)
    {
        if (host[i] == '%' && i + 2 < host.size())
        {
            const std::string_view code = host.substr(i + 1, 2);
            if (code == "25" || code == "3A" || code == "3a")
            {
                out += code == "25" ? '%' : ':';
                i += 2;
                continue;
            }
        }
        out += host[i];
    }
    return out;
}

}  // namespace runtime
