// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The GPU stop flag (run safety): the watchdog launcher creates
// research/GPU_STOP after a GPU lockup, and only the owner removes it. Every GPU entry point calls
// CheckGpuStop before any GPU work and refuses while the flag exists, reporting the file's first line
// (the watchdog's reason). scripts/preflight.sh --gpu checks the same file before a run starts;
// this is the in-process check for a flag set while the run is under way.
//
// Fail closed: the path is required (an empty or missing path refuses: no silent default); a
// missing file (ENOENT) is the only "not stopped"; a file that exists but cannot be opened or read,
// or a path whose parent is not a directory, refuses with the error. New: no source to port.
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace runtime::gpu {

struct GpuStopCheck
{
    bool stopped = true;
    std::string reason;  // the file's first line (without its line end), or why the check refused
};

inline GpuStopCheck CheckGpuStop(const char* path)
{
    if (path == nullptr || *path == '\0')
        return {true, "no GPU_STOP path given"};
    FILE* f = std::fopen(path, "r");
    if (f == nullptr)
    {
        const int err = errno;
        if (err == ENOENT)
            return {false, ""};
        return {true, std::string("GPU_STOP ") + path + " cannot be opened: " + std::strerror(err)};
    }
    char line[256] = {};
    const bool got = std::fgets(line, sizeof line, f) != nullptr;
    const bool error = std::ferror(f) != 0;
    std::fclose(f);
    if (error)
        return {true, std::string("GPU_STOP ") + path + " cannot be read"};
    if (!got)
        return {true, ""};  // present but empty: stopped, no reason given
    line[std::strcspn(line, "\r\n")] = '\0';
    return {true, line};
}

}  // namespace runtime::gpu
