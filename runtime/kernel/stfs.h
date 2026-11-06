// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Read-only STFS packages (CON/LIVE/PIRS XContent) on the host: the file table and file reads. Ported from
// survey/stfsinfo.py (M0; tested on synthetic packages and VERIFIED on the disc's packages), whose layout and
// algorithms are re-derived from Xenia xenia-canary 67f16c4e315f vfs/devices/stfs_xbox.h and
// xcontent_devices/stfs_container_device.cc (BlockToOffset, BlockToHashBlockNumber, GetBlockHash, ReadEntry).
// Fail-closed: anything inconsistent (magic, descriptor length, a chain that ends early or leaves the package)
// stops (9) with the package's path.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace runtime::stfs {

struct File
{
    std::string name;
    bool directory = false;
    int32_t parent = -1;  // index of the parent directory, -1 at the root
    uint32_t length = 0, startBlock = 0, allocatedBlocks = 0;
};

class Package
{
public:
    // Opens and checks the package at `host` and reads its file table; stops (9) on anything inconsistent.
    static std::shared_ptr<Package> Open(const std::string& host);
    const std::vector<File>& Files() const { return files_; }
    // Up to `size` bytes of file `index` from `offset` (0 at or past its end).
    uint32_t Read(uint32_t index, uint64_t offset, uint8_t* out, uint32_t size) const;
    const std::string& Host() const { return host_; }

    // The pure arithmetic (tested on hand-computed values): a data block's offset in the package, and the block
    // number of the hash table covering it at `level`, for one (read-only format) or two tables per level.
    static uint64_t BlockToOffset(uint32_t index, uint32_t perTable, uint64_t base);
    static uint32_t HashBlockNumber(uint32_t index, uint32_t level, uint32_t perTable);

private:
    std::vector<uint8_t> ReadAt(uint64_t offset, uint32_t size) const;
    uint32_t NextBlock(uint32_t index) const;
    std::string host_;
    uint64_t base_ = 0, size_ = 0;
    uint32_t perTable_ = 1, totalBlocks_ = 0;
    bool readOnly_ = true, rootActive_ = false;
    std::vector<File> files_;
    std::vector<std::vector<uint32_t>> chains_;  // each file's blocks, in order
};

}  // namespace runtime::stfs
