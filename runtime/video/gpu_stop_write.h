// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// Writes the GPU stop flag from inside the process (run safety; execution
// model §2 "Fences": a stranded submission writes research/GPU_STOP with the evidence, then the run
// stops). The first line has the shape scripts/gpu_run.sh trip() writes ("GPU_STOP written <date>
// by <writer>; only the owner clears it."), so gpu/gpu_stop.h CheckGpuStop and the launcher's
// refuse_if_stopped report it the same way. An existing flag is never truncated: the reason is
// appended to it. New: no source to port.
#pragma once
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace runtime::video {

// True when the reason reached the file. Fails (false, `error` set) for an empty path or an I/O
// error; the caller stops either way (fail closed), reporting the error.
inline bool WriteGpuStop(const char* path, const std::string& reason, std::string& error)
{
    if (path == nullptr || *path == '\0')
    {
        error = "no GPU_STOP path given";
        return false;
    }
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
    {
        error = std::string("cannot open ") + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st{};
    std::string text;
    if (::fstat(fd, &st) == 0 && st.st_size == 0)
    {
        char when[32] = {};
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &local);
        text = std::string("GPU_STOP written ") + when + " by fm4recomp (in-process); only the owner clears it.\n";
    }
    text += reason;
    text += '\n';
    size_t done = 0;
    while (done < text.size())
    {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            error = std::string("cannot write ") + path + ": " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        done += size_t(n);
    }
    const bool synced = ::fsync(fd) == 0;
    if (!(::close(fd) == 0 && synced))
    {
        error = std::string("cannot flush ") + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace runtime::video
