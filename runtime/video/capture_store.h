// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// Frame captures go only through the capped dump ring (gpu/dump_ring.h; dumps capped,
// an uncapped dump once filled ~50 GB). A capture is written to `<dir>/<prefix><id>.png` only after
// the ring accepted its size; the files of the records it evicted are deleted then. A capture larger
// than the whole byte cap is refused and nothing is written. Files are only ever created and deleted
// under the directory given (the run's evidence directory from scripts/gpu_run.sh); a name this
// store did not create is never deleted. New: no source to port.
#pragma once
#include "gpu/dump_ring.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unistd.h>

namespace runtime::video {

template <size_t Capacity>
class CaptureStore
{
public:
    // `writeFile(path)` writes exactly the bytes announced to Add and returns false on an I/O error.
    using Writer = std::function<bool(const std::string& path)>;

    CaptureStore(std::string dir, std::string prefix, uint64_t byteCap) : dir_(std::move(dir)), prefix_(std::move(prefix)), ring_(byteCap) {}

    std::string PathOf(uint64_t id) const { return dir_ + "/" + prefix_ + std::to_string(id) + ".png"; }

    // False with `error` set when refused (too large) or when a write or a deletion failed (the
    // caller stops: a capture that cannot be bounded is not taken).
    bool Add(uint64_t id, uint64_t bytes, const Writer& writeFile, std::string& error)
    {
        typename gpu::DumpRing<Capacity>::Pushed pushed = ring_.Push({id, bytes});
        for (const auto& old : pushed.evicted)
            if (::unlink(PathOf(old.id).c_str()) != 0 && errno != ENOENT)
            {
                error = "cannot delete evicted capture " + PathOf(old.id) + ": " + std::strerror(errno);
                return false;
            }
        if (!pushed.accepted)
        {
            error = "capture " + std::to_string(id) + " of " + std::to_string(bytes) + " bytes exceeds the whole byte cap";
            return false;
        }
        if (!writeFile(PathOf(id)))
        {
            error = "cannot write capture " + PathOf(id);
            return false;
        }
        return true;
    }

    size_t Count() const { return ring_.Count(); }
    uint64_t Bytes() const { return ring_.Bytes(); }

private:
    const std::string dir_, prefix_;
    gpu::DumpRing<Capacity> ring_;
};

}  // namespace runtime::video
