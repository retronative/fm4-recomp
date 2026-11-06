// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Read-only STFS packages (stfs.h).
#include "stfs.h"
#include "stops.h"
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace runtime::stfs {
namespace {
constexpr uint32_t kBlock = 0x1000, kEndOfChain = 0xFFFFFF;
constexpr uint32_t kPerLevel[3] = {170, 28900, 4913000};  // kBlocksPerHashLevel
// XContentHeader offsets (stfsinfo.py): header size, the STFS volume descriptor, the volume type.
constexpr uint32_t kHeaderSize = 0x340, kVolume = 0x379, kVolumeType = 0x3A9, kHeaderMin = 0x1712;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Le24(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
}  // namespace

uint64_t Package::BlockToOffset(uint32_t index, uint32_t perTable, uint64_t base)
{
    uint64_t block = index;
    for (const uint32_t levelBase : kPerLevel)
    {
        block += uint64_t((index + levelBase) / levelBase) * perTable;
        if (index < levelBase)
            break;
    }
    return base + (block << 12);
}

uint32_t Package::HashBlockNumber(uint32_t index, uint32_t level, uint32_t perTable)
{
    const uint32_t step[2] = {kPerLevel[0] + perTable, kPerLevel[1] + (kPerLevel[0] + 1) * perTable};
    if (level == 2)
        return step[1];
    if (index < kPerLevel[level])
        return level == 0 ? 0 : step[level - 1];
    uint32_t block = (index / kPerLevel[level]) * step[level];
    if (level == 0)
    {
        block += ((index / kPerLevel[1]) + 1) * perTable;
        if (index < kPerLevel[1])
            return block;
    }
    return block + perTable;
}

std::vector<uint8_t> Package::ReadAt(uint64_t offset, uint32_t size) const
{
    if (offset + size > size_)
        Stop(StopCode::Setup, "STFS %s: a read of %u bytes at %llX is past its end", host_.c_str(), size, (unsigned long long)offset);
    std::vector<uint8_t> data(size);
    const int fd = open(host_.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || pread(fd, data.data(), size, off_t(offset)) != ssize_t(size))
        Stop(StopCode::Setup, "STFS %s: reading %u bytes at %llX failed", host_.c_str(), size, (unsigned long long)offset);
    close(fd);
    return data;
}

uint32_t Package::NextBlock(uint32_t index) const
{
    // GetBlockHash: walk the hash levels from the top (read-only format: level 0 only, the first table).
    uint32_t secondary = rootActive_ && !readOnly_ ? kBlock : 0, levels = 0;
    for (uint32_t level = 0; level < 3; level++)
        if (totalBlocks_ < kPerLevel[level])
        {
            levels = level;
            break;
        }
    if (readOnly_)
        levels = 0;
    for (int32_t level = int32_t(levels); level >= 0; level--)
    {
        const std::vector<uint8_t> table = ReadAt(base_ + (uint64_t(HashBlockNumber(index, uint32_t(level), perTable_)) << 12) + secondary, kBlock);
        const uint32_t record = level == 0 ? index % kPerLevel[0] : (index / kPerLevel[level - 1]) % kPerLevel[0];
        const uint32_t info = Be32(&table[record * 0x18 + 0x14]);
        if (level == 0)
            return info & 0xFFFFFF;
        secondary = (info & 0x40000000) ? kBlock : 0;
    }
    return kEndOfChain;
}

std::shared_ptr<Package> Package::Open(const std::string& host)
{
    auto p = std::shared_ptr<Package>(new Package());
    p->host_ = host;
    struct stat info;
    if (stat(host.c_str(), &info) != 0)
        Stop(StopCode::Setup, "STFS %s: cannot stat", host.c_str());
    p->size_ = uint64_t(info.st_size);
    const std::vector<uint8_t> h = p->ReadAt(0, kHeaderMin);
    if (std::memcmp(h.data(), "CON ", 4) && std::memcmp(h.data(), "LIVE", 4) && std::memcmp(h.data(), "PIRS", 4))
        Stop(StopCode::Setup, "STFS %s: not an XContent package", host.c_str());
    if (Be32(&h[kVolumeType]) != 0 || h[kVolume] != 0x24)
        Stop(StopCode::Setup, "STFS %s: not an STFS volume (type %u, descriptor length %u)", host.c_str(), Be32(&h[kVolumeType]), h[kVolume]);
    const uint8_t flags = h[kVolume + 2];
    p->readOnly_ = flags & 1, p->rootActive_ = flags & 2;
    p->perTable_ = p->readOnly_ ? 1 : 2;
    p->totalBlocks_ = Be32(&h[kVolume + 0x1C]);
    p->base_ = (uint64_t(Be32(&h[kHeaderSize])) + kBlock - 1) & ~uint64_t(kBlock - 1);
    const uint32_t tableBlocks = h[kVolume + 3] | uint32_t(h[kVolume + 4]) << 8;  // host (little-endian) uint16
    uint32_t index = Le24(&h[kVolume + 5]);
    for (uint32_t n = 0; n < tableBlocks; n++)
    {
        const std::vector<uint8_t> block = p->ReadAt(BlockToOffset(index, p->perTable_, p->base_), kBlock);
        for (uint32_t m = 0; m < kBlock / 0x40; m++)
        {
            const uint8_t* e = &block[m * 0x40];
            if (e[0] == 0)
                break;
            File f;
            f.name.assign(reinterpret_cast<const char*>(e), e[40] & 0x3F);
            f.directory = e[40] & 0x80;
            const uint32_t parent = uint32_t(e[50]) << 8 | e[51];
            f.parent = parent == 0xFFFF ? -1 : int32_t(parent);
            f.length = Be32(e + 52), f.allocatedBlocks = Le24(e + 44), f.startBlock = Le24(e + 47);
            p->files_.push_back(f);
        }
        if (n + 1 < tableBlocks && (index = p->NextBlock(index)) == kEndOfChain)
            Stop(StopCode::Setup, "STFS %s: the file table chain ends after %u of %u blocks", host.c_str(), n + 1, tableBlocks);
    }
    for (const File& f : p->files_)
        if (f.parent >= int32_t(p->files_.size()) || (f.parent >= 0 && !p->files_[size_t(f.parent)].directory))
            Stop(StopCode::Setup, "STFS %s: %s names a parent that is no directory", host.c_str(), f.name.c_str());
    // Each file's block chain (ReadEntry: blocks follow level-0 hash links; the allocated count bounds it).
    for (const File& f : p->files_)
    {
        std::vector<uint32_t> chain;
        const uint32_t blocks = (f.length + kBlock - 1) / kBlock;
        for (uint32_t b = f.startBlock; !f.directory && chain.size() < blocks; b = p->NextBlock(b))
        {
            if (b == kEndOfChain || b >= p->totalBlocks_)
                Stop(StopCode::Setup, "STFS %s: %s's block chain ends after %zu of %u blocks", host.c_str(), f.name.c_str(), chain.size(), blocks);
            chain.push_back(b);
        }
        p->chains_.push_back(std::move(chain));
    }
    return p;
}

uint32_t Package::Read(uint32_t index, uint64_t offset, uint8_t* out, uint32_t size) const
{
    const File& f = files_.at(index);
    if (offset >= f.length)
        return 0;
    const uint32_t wanted = uint32_t(std::min<uint64_t>(size, f.length - offset));
    for (uint32_t done = 0; done < wanted;)
    {
        const uint64_t at = offset + done;
        const uint32_t inBlock = uint32_t(at % kBlock), n = std::min(wanted - done, kBlock - inBlock);
        const std::vector<uint8_t> data = ReadAt(BlockToOffset(chains_[index][at / kBlock], perTable_, base_) + inBlock, n);
        std::memcpy(out + done, data.data(), n);
        done += n;
    }
    return wanted;
}

}  // namespace runtime::stfs
