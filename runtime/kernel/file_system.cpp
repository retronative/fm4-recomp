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
struct MapEntry { uint32_t sector; uint64_t size; uint32_t attributes; size_t rank; };  // rank: the line (GDFX tree order)
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
        const size_t rank = discMap.size();
        if (!discMap.emplace(path, MapEntry{uint32_t(std::stoul(sector, nullptr, 16)), std::stoull(size, nullptr, 16),
                                            uint32_t(std::stoul(attributes, nullptr, 16)), rank}).second)
            Stop(StopCode::Setup, "--disc-map %s lists %s twice", map.c_str(), path.c_str());
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

uint64_t DiscAllocationSize(const DiscEntry& entry)
{
    return (entry.size + kDiscSectorSize - 1) / kDiscSectorSize * kDiscSectorSize;
}

namespace {
std::mutex rootsLock;
std::map<std::string, std::shared_ptr<const stfs::Package>> contentRoots;  // by lower-case root name
std::map<std::string, std::string> directoryRoots;                           // by lower-case root name: the host directory

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

DiscEntry PackageEntry(const std::shared_ptr<const stfs::Package>& package, int32_t at)
{
    DiscEntry entry;
    entry.host = package->Host();
    entry.package = package;
    entry.packageFile = at;
    entry.directory = at < 0 || package->Files()[size_t(at)].directory;
    entry.size = entry.directory ? 0 : package->Files()[size_t(at)].length;
    entry.attributes = entry.directory ? file_attribute::kDirectory : 0x80 | file_attribute::kReadOnly;
    return entry;
}

// A path inside a writable root's host directory: each part matched in any case; a symlink stops (9).
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
            if (EqualCase(d->d_name, part) && std::string_view(d->d_name) != "." && std::string_view(d->d_name) != "..")
                match = d->d_name, matches++;
        closedir(listing);
        if (matches == 0)
        {
            if (k + 1 == parts.size())
            {
                if (part.find(':') != std::string_view::npos)
                    Stop(StopCode::Unimplemented, "user data: the name %.*s (with ':') is not created on the host", int(part.size()), part.data());
                entry.host = host + "/" + std::string(part);
            }
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
    return Resolved::Found;
}

// "<root>:\a\b" inside a mounted package or writable root; any other device is OtherDevice.
Resolved ResolveContentPath(const std::string& device, DiscEntry& entry)
{
    const size_t colon = device.find(':');
    if (colon == std::string::npos)
        return Resolved::OtherDevice;
    std::shared_ptr<const stfs::Package> package;
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
    entry = PackageEntry(package, at);
    return Resolved::Found;
}
}  // namespace

bool MountContentRoot(const std::string& root, std::shared_ptr<const stfs::Package> package)
{
    std::lock_guard guard(rootsLock);
    return directoryRoots.count(Lower(root)) == 0 && contentRoots.emplace(Lower(root), std::move(package)).second;
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
    entry.relative = relative;
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

std::vector<DirectoryChild> ListDirectory(const DiscEntry& directory)
{
    std::vector<DirectoryChild> children;
    if (!directory.directory)
        return children;
    if (directory.writable)
        Stop(StopCode::Unimplemented, "listing the writable content directory %s is not modelled yet", directory.host.c_str());
    if (directory.package)
    {
        const auto& files = directory.package->Files();
        for (size_t i = 0; i < files.size(); i++)
            if (files[i].parent == directory.packageFile)
                children.push_back({files[i].name, PackageEntry(directory.package, int32_t(i))});
        return children;
    }
    const std::string prefix = directory.relative.empty() ? "" : directory.relative + "/";
    std::vector<std::pair<size_t, DirectoryChild>> ranked;
    for (const auto& [path, m] : discMap)
    {
        if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0 || path.find('/', prefix.size()) != std::string::npos)
            continue;
        DiscEntry e;
        e.host = discRoot + "/" + path;
        e.relative = path;
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
    if (!entry.writable || entry.directory)
        Stop(StopCode::Unimplemented, "removing %s: not a file in a writable content root", entry.host.c_str());
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
    const uint32_t body = ArenaAlloc(base, 16);
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
