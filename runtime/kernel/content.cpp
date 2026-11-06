// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Saved content storage (content.h).
#include "content.h"
#include "stops.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
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

}  // namespace runtime
