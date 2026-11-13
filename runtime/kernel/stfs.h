// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Read-only STFS packages (CON/LIVE/PIRS XContent) on the host: the file table and file reads. Ported from
// survey/stfsinfo.py (M0; tested on synthetic packages and VERIFIED on the disc's packages), whose layout and
// algorithms are re-derived from Xenia xenia-canary 67f16c4e315f vfs/devices/stfs_xbox.h and
// xcontent_devices/stfs_container_device.cc (BlockToOffset, BlockToHashBlockNumber, GetBlockHash, ReadEntry).
// Fail-closed: anything inconsistent (magic, descriptor length, a chain that ends early or leaves the package)
// stops (9) with the package's path.
//
// FM4 Recomp (Disc 2): the SHA-1 hash tree is verified, after the data side's extractor
// (survey/stfs_extract.cpp: its Package::Table / HashEntry / DataBlock, ported here; the rules are
// Velocity 1690428a StfsPackage.cpp facts, cited there): the top hash table against the volume descriptor's hash (+0x08),
// each lower table against its entry in the table above, each data block against its level-0 entry, a hash over the
// whole 0x1000-byte block. The top level is 0 up to 0xAA blocks, 1 up to 0x70E4, 2 up to 0x4AF768 (Velocity
// CalcualateTopLevel). Tables are verified once and kept (the file table's and every chain's at Open); data blocks on
// every read. Only read-only packages (one table per level) are opened; a mismatch stops (9).
#pragma once
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
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
    // Opens and checks the package at `host`, verifies its hash tables and reads its file table; stops (9) on anything
    // inconsistent. The file stays open (read-only) for the package's life.
    static std::shared_ptr<Package> Open(const std::string& host);
    ~Package();
    // Whether `host` is a complete read-only STFS package of `contentType` (header magic, STFS volume, read-only, the
    // content type at 0x344, and a length reaching the end of its last data block), reading its header only. A package
    // being copied (FM4's installer writes it front to back) is shorter and is not complete. `problem` names the first
    // failure; `displayName` (if given, 256 bytes) receives the first locale's display name (0x411, 128 UTF-16BE characters).
    static bool Complete(const std::string& host, uint32_t contentType, std::string& problem, uint8_t* displayName = nullptr);
    const std::vector<File>& Files() const { return files_; }
    // Up to `size` bytes of file `index` from `offset` (0 at or past its end); every data block it touches is verified.
    uint32_t Read(uint32_t index, uint64_t offset, uint8_t* out, uint32_t size) const;
    const std::string& Host() const { return host_; }

    // The pure arithmetic (tested on hand-computed values): a data block's offset in the package, and the block
    // number of the hash table covering it at `level`, for one (read-only format) or two tables per level.
    static uint64_t BlockToOffset(uint32_t index, uint32_t perTable, uint64_t base);
    static uint32_t HashBlockNumber(uint32_t index, uint32_t level, uint32_t perTable);
    // Velocity's top level for a volume of `blocks` data blocks: 0, 1 or 2; -1 past a level-2 tree.
    static int TopLevel(uint32_t blocks);

private:
    Package() = default;
    std::vector<uint8_t> ReadAt(uint64_t offset, uint32_t size) const;
    const std::vector<uint8_t>& Table(int level, uint32_t number) const;  // verified, cached; needs tablesLock_
    const uint8_t* HashEntry(uint32_t index) const;                      // the level-0 entry of a data block
    uint32_t NextBlock(uint32_t index) const;
    std::string host_;
    int fd_ = -1;
    uint64_t base_ = 0, size_ = 0;
    uint32_t totalBlocks_ = 0;
    int top_ = 0;
    uint8_t topHash_[20] = {};
    std::vector<File> files_;
    std::vector<std::vector<uint32_t>> chains_;  // each file's blocks, in order
    mutable std::recursive_mutex tablesLock_;
    mutable std::map<std::pair<int, uint32_t>, std::vector<uint8_t>> tables_;  // verified hash tables by (level, number)
};

}  // namespace runtime::stfs
