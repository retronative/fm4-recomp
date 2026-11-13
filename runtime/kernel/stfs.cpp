// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Read-only STFS packages (stfs.h). FM4 Recomp (Disc 2): the hash-tree verification (Table, HashEntry, the
// check in Read) is ported from the data side's drafted survey/stfs_extract.cpp (Package::Table/HashEntry/DataBlock,
// same project and licence); its SHA-1 is TinySHA1 (XenonRecomp's pinned thirdparty, ISC-style licence kept in its
// header). The file stays open for the package's life instead of one open per read.
#include "stfs.h"
#include "stops.h"
#include <TinySHA1.hpp>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace runtime::stfs {
namespace {
constexpr uint32_t kBlock = 0x1000, kEndOfChain = 0xFFFFFF;
constexpr uint32_t kPerLevel[3] = {170, 28900, 4913000};  // kBlocksPerHashLevel
constexpr uint32_t kTopLimits[3] = {0xAA, 0x70E4, 0x4AF768};  // Velocity CalcualateTopLevel
// XContentHeader offsets (stfsinfo.py): header size, the content type, the STFS volume descriptor (its top hash table's
// SHA-1 at +0x08, total blocks at +0x1C), the volume type, the display name.
constexpr uint32_t kHeaderSize = 0x340, kContentType = 0x344, kVolume = 0x379, kVolumeType = 0x3A9, kDisplayName = 0x411,
                   kHeaderMin = 0x1712;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Le24(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

bool Sha1Matches(const uint8_t* data, size_t size, const uint8_t* want)
{
    sha1::SHA1 s;
    s.processBytes(data, size);
    uint8_t digest[20];
    s.finalize(digest);  // digest8_t: the 20 bytes, big-endian words
    return std::memcmp(digest, want, 20) == 0;
}
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

int Package::TopLevel(uint32_t blocks)
{
    for (int level = 0; level < 3; level++)
        if (blocks <= kTopLimits[level])
            return level;
    return -1;
}

Package::~Package()
{
    if (fd_ >= 0)
        close(fd_);
}

std::vector<uint8_t> Package::ReadAt(uint64_t offset, uint32_t size) const
{
    if (offset + size > size_)
        Stop(StopCode::Setup, "STFS %s: a read of %u bytes at %llX is past its end", host_.c_str(), size, (unsigned long long)offset);
    std::vector<uint8_t> data(size);
    for (uint32_t got = 0; got < size;)
    {
        const ssize_t n = pread(fd_, data.data() + got, size - got, off_t(offset + got));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            Stop(StopCode::Setup, "STFS %s: reading %u bytes at %llX failed", host_.c_str(), size, (unsigned long long)offset);
        got += uint32_t(n);
    }
    return data;
}

const std::vector<uint8_t>& Package::Table(int level, uint32_t number) const
{
    std::lock_guard guard(tablesLock_);
    const auto key = std::make_pair(level, number);
    if (const auto it = tables_.find(key); it != tables_.end())
        return it->second;
    const uint8_t* want = topHash_;
    if (level == top_)
    {
        if (number != 0)
            Stop(StopCode::Setup, "STFS %s: level-%d table %u is past the top table", host_.c_str(), level, number);
    }
    else
        want = Table(level + 1, number / kPerLevel[0]).data() + (number % kPerLevel[0]) * 0x18;
    const uint32_t physical = HashBlockNumber(uint32_t(uint64_t(number) * kPerLevel[level]), uint32_t(level), 1);
    std::vector<uint8_t> data = ReadAt(base_ + (uint64_t(physical) << 12), kBlock);
    if (!Sha1Matches(data.data(), data.size(), want))
        Stop(StopCode::Setup, "STFS %s: the level-%d hash table %u (block %u) does not match its hash", host_.c_str(), level, number, physical);
    return tables_.emplace(key, std::move(data)).first->second;
}

const uint8_t* Package::HashEntry(uint32_t index) const
{
    if (index >= totalBlocks_)
        Stop(StopCode::Setup, "STFS %s: data block %u is past the volume (%u blocks)", host_.c_str(), index, totalBlocks_);
    return Table(0, index / kPerLevel[0]).data() + (index % kPerLevel[0]) * 0x18;
}

uint32_t Package::NextBlock(uint32_t index) const
{
    return Be32(HashEntry(index) + 0x14) & 0xFFFFFF;  // level0_next_block (Xenia stfs_xbox.h; read-only: level 0 only)
}

bool Package::Complete(const std::string& host, uint32_t contentType, std::string& problem, uint8_t* displayName)
{
    struct stat info;
    if (lstat(host.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return problem = "not a regular file", false;
    const int fd = open(host.c_str(), O_RDONLY | O_NOFOLLOW);
    std::vector<uint8_t> h(kHeaderMin);
    const bool read = fd >= 0 && pread(fd, h.data(), h.size(), 0) == ssize_t(h.size());
    if (fd >= 0)
        close(fd);
    if (!read)
        return problem = "shorter than an XContent header", false;
    if (std::memcmp(h.data(), "CON ", 4) && std::memcmp(h.data(), "LIVE", 4) && std::memcmp(h.data(), "PIRS", 4))
        return problem = "not an XContent package", false;
    if (Be32(&h[kVolumeType]) != 0 || h[kVolume] != 0x24 || (h[kVolume + 2] & 1) == 0)
        return problem = "not a read-only STFS volume", false;
    if (Be32(&h[kContentType]) != contentType)
        return problem = "another content type", false;
    const uint32_t blocks = Be32(&h[kVolume + 0x1C]);
    const uint64_t base = (uint64_t(Be32(&h[kHeaderSize])) + kBlock - 1) & ~uint64_t(kBlock - 1);
    const uint64_t end = blocks == 0 ? base : BlockToOffset(blocks - 1, 1, base) + kBlock;
    if (uint64_t(info.st_size) < end)
        return problem = "incomplete (" + std::to_string(info.st_size) + " of " + std::to_string(end) + " bytes)", false;
    if (displayName)
        std::memcpy(displayName, &h[kDisplayName], 256);
    return true;
}

std::shared_ptr<Package> Package::Open(const std::string& host)
{
    auto p = std::shared_ptr<Package>(new Package());
    p->host_ = host;
    p->fd_ = open(host.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat info;
    if (p->fd_ < 0 || fstat(p->fd_, &info) != 0)
        Stop(StopCode::Setup, "STFS %s: cannot open", host.c_str());
    p->size_ = uint64_t(info.st_size);
    const std::vector<uint8_t> h = p->ReadAt(0, kHeaderMin);
    if (std::memcmp(h.data(), "CON ", 4) && std::memcmp(h.data(), "LIVE", 4) && std::memcmp(h.data(), "PIRS", 4))
        Stop(StopCode::Setup, "STFS %s: not an XContent package", host.c_str());
    if (Be32(&h[kVolumeType]) != 0 || h[kVolume] != 0x24)
        Stop(StopCode::Setup, "STFS %s: not an STFS volume (type %u, descriptor length %u)", host.c_str(), Be32(&h[kVolumeType]), h[kVolume]);
    if ((h[kVolume + 2] & 1) == 0)
        Stop(StopCode::Setup, "STFS %s: not a read-only package (two tables per level: not modelled)", host.c_str());
    p->totalBlocks_ = Be32(&h[kVolume + 0x1C]);
    p->top_ = TopLevel(p->totalBlocks_);
    if (p->top_ < 0)
        Stop(StopCode::Setup, "STFS %s: %u blocks need more than three hash levels", host.c_str(), p->totalBlocks_);
    std::memcpy(p->topHash_, &h[kVolume + 8], 20);
    p->base_ = (uint64_t(Be32(&h[kHeaderSize])) + kBlock - 1) & ~uint64_t(kBlock - 1);
    const uint32_t tableBlocks = h[kVolume + 3] | uint32_t(h[kVolume + 4]) << 8;  // host (little-endian) uint16
    uint32_t index = Le24(&h[kVolume + 5]);
    for (uint32_t n = 0; n < tableBlocks; n++)
    {
        uint8_t block[kBlock];
        (void)p->Read(UINT32_MAX, uint64_t(index) * kBlock, block, kBlock);  // a verified data block (the file table's)
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
    // index UINT32_MAX (Open only): raw data blocks from block offset / 0x1000 (the file table), still verified.
    const bool raw = index == UINT32_MAX;
    const uint64_t length = raw ? uint64_t(totalBlocks_) * kBlock : files_.at(index).length;
    if (offset >= length)
        return 0;
    const uint32_t wanted = uint32_t(std::min<uint64_t>(size, length - offset));
    for (uint32_t done = 0; done < wanted;)
    {
        const uint64_t at = offset + done;
        const uint32_t inBlock = uint32_t(at % kBlock), n = std::min(wanted - done, kBlock - inBlock);
        const uint32_t block = raw ? uint32_t(at / kBlock) : chains_[index][at / kBlock];
        uint8_t want[20];
        std::memcpy(want, HashEntry(block), 20);
        const std::vector<uint8_t> data = ReadAt(BlockToOffset(block, 1, base_), kBlock);  // the whole block: its hash covers all of it
        if (!Sha1Matches(data.data(), data.size(), want))
            Stop(StopCode::Setup, "STFS %s: data block %u does not match its level-0 hash", host_.c_str(), block);
        std::memcpy(out + done, data.data() + inBlock, n);
        done += n;
    }
    return wanted;
}

}  // namespace runtime::stfs
