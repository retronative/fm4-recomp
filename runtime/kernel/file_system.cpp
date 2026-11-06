// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The title's disc, read-only (file_system.h).
#include "file_system.h"
#include "arena.h"
#include "objects.h"
#include "stops.h"
#include "stfs.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <cctype>
#include <climits>
#include <cstdlib>
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

std::string discRoot;  // realpath of the extracted disc
struct MapEntry { uint32_t sector; uint64_t size; uint32_t attributes; };
std::unordered_map<std::string, MapEntry> discMap;  // by path relative to the root, '/'-separated, disc case
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

}  // namespace

void MountDisc(const std::string& root, const std::string& map)
{
    std::ifstream in(map);
    if (!in)
        Stop(StopCode::Setup, "--disc-map %s cannot be read (survey/gdfx.py writes it)", map.c_str());
    discMap.clear();
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string path, sector, size, attributes;
        if (!std::getline(fields, path, '\t') || !std::getline(fields, sector, '\t') || !std::getline(fields, size, '\t') ||
            !std::getline(fields, attributes) || path.empty())
            Stop(StopCode::Setup, "--disc-map %s: a malformed line: %.80s", map.c_str(), line.c_str());
        discMap[path] = MapEntry{uint32_t(std::stoul(sector, nullptr, 16)), std::stoull(size, nullptr, 16), uint32_t(std::stoul(attributes, nullptr, 16))};
    }
    if (discMap.empty())
        Stop(StopCode::Setup, "--disc-map %s is empty", map.c_str());
    char resolved[PATH_MAX];
    struct stat info;
    if (realpath(root.c_str(), resolved) == nullptr || stat(resolved, &info) != 0 || !S_ISDIR(info.st_mode))
        Stop(StopCode::Setup, "--disc %s is not a directory (the extracted Disc 1, game/files)", root.c_str());
    discRoot = resolved;
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

bool ValidGuestPath(std::string_view path)
{
    for (const char c : path)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 31 || u >= 127)
            return false;
        switch (c)
        {
        case '"': case '*': case '+': case ',': case ';': case '<': case '=': case '>': case '?': case '|':
            return false;
        default:
            break;
        }
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

uint64_t DiscAllocationSize(const DiscEntry& entry)
{
    return (entry.size + kDiscSectorSize - 1) / kDiscSectorSize * kDiscSectorSize;
}

namespace {
std::mutex rootsLock;
std::map<std::string, std::shared_ptr<const stfs::Package>> contentRoots;  // by lower-case root name

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// "<root>:\a\b" inside a mounted package; any other device is OtherDevice.
Resolved ResolveContentPath(const std::string& device, DiscEntry& entry)
{
    const size_t colon = device.find(':');
    if (colon == std::string::npos)
        return Resolved::OtherDevice;
    std::shared_ptr<const stfs::Package> package;
    {
        std::lock_guard guard(rootsLock);
        const auto it = contentRoots.find(Lower(std::string_view(device).substr(0, colon)));
        if (it == contentRoots.end())
            return Resolved::OtherDevice;
        package = it->second;
    }
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
    entry = DiscEntry{};
    entry.host = package->Host();
    entry.package = package;
    entry.packageFile = at;
    entry.directory = at < 0 || package->Files()[size_t(at)].directory;
    entry.size = entry.directory ? 0 : package->Files()[size_t(at)].length;
    entry.attributes = entry.directory ? file_attribute::kDirectory : 0x80 | file_attribute::kReadOnly;
    return Resolved::Found;
}
}  // namespace

bool MountContentRoot(const std::string& root, std::shared_ptr<const stfs::Package> package)
{
    std::lock_guard guard(rootsLock);
    return contentRoots.emplace(Lower(root), std::move(package)).second;
}

bool UnmountContentRoot(const std::string& root)
{
    std::lock_guard guard(rootsLock);
    return contentRoots.erase(Lower(root)) == 1;
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
    if (discRoot.empty())
        Stop(StopCode::Setup, "no disc is mounted (--disc)");

    std::string host = discRoot, relative;
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
    if (relative.empty())  // the root (Xenia disc_image_device.cc:116: directory, no other attribute)
    {
        entry.size = 0, entry.sector = 0, entry.attributes = file_attribute::kDirectory;
        return Resolved::Found;
    }
    const auto it = discMap.find(relative);
    if (it == discMap.end() || ((it->second.attributes & file_attribute::kDirectory) != 0) != directory ||
        (!directory && it->second.size != entry.size))
        Stop(StopCode::Setup, "%s on the disc does not match the disc map (not listed, or another kind or size)", relative.c_str());
    entry.size = it->second.size;  // a directory: its directory data size (Xenia: the dirent length)
    entry.sector = it->second.sector;
    entry.attributes = it->second.attributes | file_attribute::kReadOnly;  // (:146)
    return Resolved::Found;
}

namespace {
std::mutex filesLock;
std::map<uint32_t, OpenFile> files;  // by object body
}  // namespace

uint32_t OpenDiscFile(uint8_t* base, const DiscEntry& entry, bool synchronous)
{
    const uint32_t body = ArenaAlloc(base, 16);
    {
        std::lock_guard guard(filesLock);
        files[body] = OpenFile{entry, 0, synchronous};
    }
    return CreateObject(ObjectType::File, body, "");
}

OpenFile* FindOpenFile(uint32_t body)
{
    std::lock_guard guard(filesLock);
    const auto it = files.find(body);
    return it == files.end() ? nullptr : &it->second;
}

uint32_t ReadDiscFile(const DiscEntry& entry, uint64_t offset, uint8_t* buffer, uint32_t size)
{
    if (entry.package)
        return entry.packageFile < 0 ? 0 : entry.package->Read(uint32_t(entry.packageFile), offset, buffer, size);
    if (offset >= entry.size)
        return 0;
    const uint32_t wanted = uint32_t(std::min<uint64_t>(size, entry.size - offset));
    const int fd = open(entry.host.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        Stop(StopCode::Setup, "cannot open %s on the disc for reading", entry.host.c_str());
    uint32_t done = 0;
    while (done < wanted)
    {
        const ssize_t n = pread(fd, buffer + done, wanted - done, off_t(offset + done));
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

}  // namespace runtime
