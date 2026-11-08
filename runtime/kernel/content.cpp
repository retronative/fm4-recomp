// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Saved content storage (content.h).
#include "content.h"
#include <unistd.h>
#include <vector>
#include "stops.h"
#include <algorithm>
#include <cerrno>
#include "file_system.h"
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>

namespace runtime {
namespace {

std::string userData;

std::string TypeDirectory(uint32_t contentType)
{
    char type[16];
    std::snprintf(type, sizeof type, "%08X", contentType);
    return userData + "/content/" + std::to_string(kContentDeviceId) + "/" + type;
}

uint64_t BytesUnder(const std::string& path)
{
    struct stat info;
    if (lstat(path.c_str(), &info) != 0)
        return 0;
    if (S_ISLNK(info.st_mode))
        Stop(StopCode::Setup, "user data: %s is a symlink", path.c_str());
    if (!S_ISDIR(info.st_mode))
        return uint64_t(info.st_size);
    uint64_t total = 0;
    if (DIR* d = opendir(path.c_str()))
    {
        while (const dirent* e = readdir(d))
            if (std::string_view(e->d_name) != "." && std::string_view(e->d_name) != "..")
                total += BytesUnder(path + "/" + e->d_name);
        closedir(d);
    }
    return total;
}

}  // namespace

void MountUserData(const std::string& root)
{
    char resolved[PATH_MAX];
    struct stat info;
    if (lstat(root.c_str(), &info) != 0 || S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode) || realpath(root.c_str(), resolved) == nullptr)
        Stop(StopCode::Setup, "--user-data %s is not a directory (a symlink is refused)", root.c_str());
    userData = resolved;
}

const std::string& UserDataRoot() { return userData; }

std::vector<ContentData> ListContent(uint32_t contentType)
{
    if (userData.empty())
        Stop(StopCode::Setup, "no user-data directory is mounted (--user-data)");
    std::vector<std::string> names;
    const std::string directory = TypeDirectory(contentType);
    if (DIR* d = opendir(directory.c_str()))
    {
        while (const dirent* e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.size() > 9 && name.compare(name.size() - 9, 9, ".xcontent") == 0)
                names.push_back(name);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    std::vector<ContentData> items;
    for (const std::string& name : names)
    {
        std::ifstream in(directory + "/" + name, std::ios::binary);
        ContentData data{};
        in.read(reinterpret_cast<char*>(data.data()), data.size());
        if (in.gcount() != std::streamsize(data.size()) || in.peek() != EOF)
            Stop(StopCode::Setup, "user data: %s/%s is not a %u-byte content record", directory.c_str(), name.c_str(), kContentDataSize);
        items.push_back(data);
    }
    return items;
}

uint64_t ContentBytesUsed() { return userData.empty() ? 0 : BytesUnder(userData + "/content"); }

namespace {
bool SafeComponent(const std::string& name)
{
    bool safe = !name.empty() && name != "." && name != "..";
    for (const char c : name)
        safe = safe && c > 31 && c < 127 && c != '/' && c != '\\' && c != ':';
    return safe;
}

uint32_t ContentType(const ContentData& data)
{
    return uint32_t(data[4]) << 24 | uint32_t(data[5]) << 16 | uint32_t(data[6]) << 8 | data[7];
}
}  // namespace

std::string ContentFileName(const ContentData& data)
{
    const char* at = reinterpret_cast<const char*>(data.data() + 0x108);
    const std::string name(at, strnlen(at, 42));
    if (!SafeComponent(name))
        Stop(StopCode::Unimplemented, "content file name \"%s\" is not a single safe path component", name.c_str());
    return name;
}

bool ContentExists(const ContentData& data)
{
    if (userData.empty())
        Stop(StopCode::Setup, "no user-data directory is mounted (--user-data)");
    struct stat info;
    const std::string path = TypeDirectory(ContentType(data)) + "/" + ContentFileName(data);
    if (lstat(path.c_str(), &info) != 0)
        return false;
    if (S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode))
        Stop(StopCode::Setup, "user data: %s is not a directory (a symlink is refused)", path.c_str());
    return true;
}

}  // namespace runtime

namespace runtime {

uint32_t CreateContent(const ContentData& data, const std::string& root)
{
    constexpr uint32_t kAlreadyExists = 0xB7, kAccessDenied = 5;
    if (!SafeComponent(root))
        Stop(StopCode::Unimplemented, "content root name \"%s\" is not a single safe path component", root.c_str());
    if (ContentRootMounted(root) || ContentExists(data))
        return kAlreadyExists;
    const std::string typeDirectory = TypeDirectory(ContentType(data)), item = typeDirectory + "/" + ContentFileName(data);
    for (const std::string& d : {userData + "/content", userData + "/content/" + std::to_string(kContentDeviceId), typeDirectory})
        if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST)
            return kAccessDenied;
    if (mkdir(item.c_str(), 0755) != 0)
        return kAccessDenied;
    const std::string record = item + ".xcontent";
    std::ofstream out(record, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    out.close();
    if (!out)
        Stop(StopCode::Setup, "user data: cannot write %s", record.c_str());
    if (!MountDirectoryRoot(root, item))
        Stop(StopCode::Setup, "content root %s could not be mounted", root.c_str());
    return 0;
}

namespace {
void RemoveTree(const std::string& path)
{
    struct stat info;
    if (lstat(path.c_str(), &info) != 0 || S_ISLNK(info.st_mode) || !(S_ISDIR(info.st_mode) || S_ISREG(info.st_mode)))
        Stop(StopCode::Setup, "user data: %s is gone, a symbolic link or not a file or directory", path.c_str());
    if (S_ISDIR(info.st_mode))
    {
        std::vector<std::string> children;
        if (DIR* dir = opendir(path.c_str()))
        {
            while (const dirent* e = readdir(dir))
                if (std::string(e->d_name) != "." && std::string(e->d_name) != "..")
                    children.push_back(path + "/" + e->d_name);
            closedir(dir);
        }
        for (const std::string& child : children)
            RemoveTree(child);
        if (rmdir(path.c_str()) != 0)
            Stop(StopCode::Setup, "user data: cannot remove the directory %s", path.c_str());
    }
    else if (unlink(path.c_str()) != 0)
        Stop(StopCode::Setup, "user data: cannot remove %s", path.c_str());
}
}  // namespace

uint32_t DeleteContent(const ContentData& data)
{
    constexpr uint32_t kFileNotFound = 2, kAccessDenied = 5;
    const std::string item = TypeDirectory(ContentType(data)) + "/" + ContentFileName(data);
    if (DirectoryRootMountedAt(item))
        return kAccessDenied;
    if (!ContentExists(data))
        return kFileNotFound;
    RemoveTree(item);
    const std::string record = item + ".xcontent";
    struct stat info;
    if (lstat(record.c_str(), &info) == 0)
        RemoveTree(record);
    return 0;
}

uint32_t OpenContent(const ContentData& data, const std::string& root)
{
    constexpr uint32_t kAlreadyExists = 0xB7, kFileNotFound = 2;
    if (!SafeComponent(root))
        Stop(StopCode::Unimplemented, "content root name \"%s\" is not a single safe path component", root.c_str());
    if (ContentRootMounted(root))
        return kAlreadyExists;
    if (!ContentExists(data))
        return kFileNotFound;
    if (!MountDirectoryRoot(root, TypeDirectory(ContentType(data)) + "/" + ContentFileName(data)))
        Stop(StopCode::Setup, "content root %s could not be mounted", root.c_str());
    return 0;
}

}  // namespace runtime
