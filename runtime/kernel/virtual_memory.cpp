// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/virtual_memory.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
#include "virtual_memory.h"
#include "stops.h"
#include "guest_memory.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace runtime {
namespace {

std::recursive_mutex lock;
std::unique_ptr<Heap> v00000000, v40000000, vA0000000, vC0000000, vE0000000;
// Which physical view holds each 4 KB physical frame (by its base address; 0 none), under `lock`.
constexpr uint32_t kFrame = 4096;
std::vector<uint32_t> frames(kPhysicalSize / kFrame);

bool Writable(uint32_t protect)
{
    return protect & (page::kReadWrite | page::kWriteCopy | page::kExecuteReadWrite | page::kExecuteWriteCopy);
}
bool Readable(uint32_t protect) { return !(protect & page::kNoAccess) && protect != 0; }

}  // namespace

Heap::Heap(uint8_t* membase, uint32_t base, uint32_t size, uint32_t pageSize, Kind kind, uint32_t physicalBase)
    : membase_(membase), base_(base), pageSize_(pageSize), kind_(kind), physicalBase_(physicalBase), pages_(size / pageSize) {}

bool Heap::Claim(uint32_t first, uint32_t count, bool take)
{
    // The physical frames of pages [first, first + count) of a physical view. Taking fails (and
    // changes nothing) if another view holds any of them; releasing frees them.
    if (kind_ == Kind::Virtual)
        return true;
    const uint32_t begin = (physicalBase_ + first * pageSize_) / kFrame, end = begin + count * (pageSize_ / kFrame);
    for (uint32_t f = begin; f < end && take; f++)
        if (frames[f] != 0 && frames[f] != base_)
            return false;
    for (uint32_t f = begin; f < end; f++)
        frames[f] = take ? base_ : 0;
    return true;
}

void Heap::SetHostAccess(uint32_t first, uint32_t count)
{
    // Most permissive access of the guest pages sharing each host page.
    const uint64_t host = uint64_t(getpagesize());
    const uint64_t start = (uint64_t(base_) + uint64_t(first) * pageSize_) & ~(host - 1);
    const uint64_t end = (uint64_t(base_) + uint64_t(first + count) * pageSize_ + host - 1) & ~(host - 1);
    for (uint64_t h = start; h < end; h += host)
    {
        int access = PROT_NONE;
        for (uint64_t a = h; a < h + host; a += pageSize_ < host ? pageSize_ : host)
        {
            if (a < base_ || a - base_ >= size())
                continue;
            const Page& p = pages_[(a - base_) / pageSize_];
            if (p.state & mem::kCommit)
                access |= Writable(p.protect) ? PROT_READ | PROT_WRITE : Readable(p.protect) ? PROT_READ : PROT_NONE;
        }
        if (mprotect(membase_ + h, host, access) != 0)
            Stop(StopCode::Setup, "cannot set guest page access at %08llX", (unsigned long long)h);
        // Physical memory is seen through every view: the same access at the same offset of the
        // other view (each frame belongs to one view's allocation, so no other page decides it).
        for (const uint32_t view : {0xA0000000u, 0xC0000000u})
            if (kind_ == Kind::SharedPhysical && view != base_ && mprotect(membase_ + view + (h - base_), host, access) != 0)
                Stop(StopCode::Setup, "cannot set guest page access at %08llX", (unsigned long long)(view + (h - base_)));
    }
}

void Heap::Zero(uint32_t first, uint32_t count)
{
    // Pages leave the committed state zeroed, so a later commit reads zeros (as on the console);
    // the host pages are writable for the clear, then get their computed access back.
    const uint64_t host = uint64_t(getpagesize());
    const uint64_t address = uint64_t(base_) + uint64_t(first) * pageSize_, size = uint64_t(count) * pageSize_;
    const uint64_t start = address & ~(host - 1), end = (address + size + host - 1) & ~(host - 1);
    if (mprotect(membase_ + start, end - start, PROT_READ | PROT_WRITE) != 0)
        Stop(StopCode::Setup, "cannot clear guest memory at %08llX", (unsigned long long)address);
    std::memset(membase_ + address, 0, size);
}

bool Heap::AllocFixed(uint32_t address, uint32_t size, uint32_t type, uint32_t protect)
{
    std::lock_guard guard(lock);
    if (!Contains(address) || (address - base_) % pageSize_ || size == 0)
        return false;
    const uint32_t first = (address - base_) / pageSize_, count = (size + pageSize_ - 1) / pageSize_;
    if (first + count > pages_.size())
        return false;
    for (uint32_t i = first; i < first + count; i++)
    {
        const bool free = pages_[i].state == 0;
        if ((type & mem::kReserve) ? !free : free)  // reserve needs free pages, commit-only needs reserved ones
            return false;
    }
    if ((type & mem::kReserve) && !Claim(first, count, true))  // physical memory another view holds
        return false;
    for (uint32_t i = first; i < first + count; i++)
    {
        Page& p = pages_[i];
        if (type & mem::kReserve)
            p = {first, count, mem::kReserve, protect, protect};
        if (type & mem::kCommit)
        {
            p.state |= mem::kCommit;
            p.protect = protect;
        }
    }
    SetHostAccess(first, count);
    return true;
}

uint32_t Heap::Alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool topDown)
{
    std::lock_guard guard(lock);
    const uint32_t count = (size + pageSize_ - 1) / pageSize_;
    const uint32_t step = alignment > pageSize_ ? alignment / pageSize_ : 1;
    if (count == 0 || count > pages_.size())
        return 0;
    auto fits = [&](uint32_t first) {
        for (uint32_t i = first; i < first + count; i++)
            if (pages_[i].state != 0)
                return false;
        return true;
    };
    const uint32_t last = uint32_t(pages_.size()) - count;
    for (uint32_t k = 0; k <= last / step; k++)
    {
        const uint32_t first = topDown ? (last / step - k) * step : k * step;
        if (fits(first) && AllocFixed(base_ + first * pageSize_, count * pageSize_, type | mem::kReserve, protect))
            return base_ + first * pageSize_;
    }
    return 0;
}

bool Heap::Release(uint32_t address, uint32_t* releasedSize)
{
    std::lock_guard guard(lock);
    if (!Contains(address))
        return false;
    const uint32_t index = (address - base_) / pageSize_;
    const Page p = pages_[index];
    if (p.state == 0 || p.allocationBase != index)
        return false;  // MEM_RELEASE takes the allocation's base address
    Zero(index, p.allocationPages);
    Claim(index, p.allocationPages, false);
    for (uint32_t i = index; i < index + p.allocationPages; i++)
        pages_[i] = {};
    SetHostAccess(index, p.allocationPages);
    if (releasedSize)
        *releasedSize = p.allocationPages * pageSize_;
    return true;
}

bool Heap::Decommit(uint32_t address, uint32_t size)
{
    std::lock_guard guard(lock);
    if (!Contains(address) || size == 0 || size - 1 > base_ + this->size() - 1 - address)  // past the heap (or wrapping): nothing is freed
        return false;
    const uint32_t first = (address - base_) / pageSize_;
    const uint32_t count = (address + size - 1 - base_) / pageSize_ - first + 1;
    if (first + count > pages_.size())
        return false;
    for (uint32_t i = first; i < first + count; i++)
        if (pages_[i].state == 0)
            return false;
    Zero(first, count);
    for (uint32_t i = first; i < first + count; i++)
        pages_[i].state = mem::kReserve;
    SetHostAccess(first, count);
    return true;
}

bool Heap::Protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t* oldProtect)
{
    std::lock_guard guard(lock);
    if (!Contains(address) || size == 0 || size - 1 > base_ + this->size() - 1 - address)  // past the heap (or wrapping): nothing changes
        return false;
    const uint32_t first = (address - base_) / pageSize_;
    const uint32_t count = (address + size - 1 - base_) / pageSize_ - first + 1;
    if (first + count > pages_.size())
        return false;
    for (uint32_t i = first; i < first + count; i++)
        if (!(pages_[i].state & mem::kCommit))
            return false;
    if (oldProtect)
        *oldProtect = pages_[first].protect;
    for (uint32_t i = first; i < first + count; i++)
        pages_[i].protect = protect;
    SetHostAccess(first, count);
    return true;
}

bool Heap::Query(uint32_t address, RegionInfo& info) const
{
    std::lock_guard guard(lock);
    if (!Contains(address))
        return false;
    const uint32_t index = (address - base_) / pageSize_;
    const Page& p = pages_[index];
    info = {};
    info.base = base_ + index * pageSize_;
    info.state = p.state ? (p.state & mem::kCommit ? mem::kCommit : mem::kReserve) : mem::kFree;
    // The region: following pages in the same state (and allocation).
    uint32_t end = index;
    while (end < pages_.size() && (pages_[end].state ? (pages_[end].state & mem::kCommit ? mem::kCommit : mem::kReserve)
                                                      : mem::kFree) == info.state &&
           pages_[end].allocationBase == p.allocationBase && pages_[end].protect == p.protect)
        end++;
    info.size = (end - index) * pageSize_;
    if (p.state)
    {
        info.allocationBase = base_ + p.allocationBase * pageSize_;
        info.allocationProtect = p.allocationProtect;
        info.protect = p.state & mem::kCommit ? p.protect : 0;
    }
    return true;
}

bool Heap::IsCommitted(uint32_t address, uint32_t size) const
{
    std::lock_guard guard(lock);
    if (!Contains(address) || size == 0 || size - 1 > base_ + this->size() - 1 - address)
        return false;
    for (uint32_t i = (address - base_) / pageSize_; i <= (address + size - 1 - base_) / pageSize_; i++)
        if (!(pages_[i].state & mem::kCommit))
            return false;
    return true;
}

void InitializeHeaps(uint8_t* membase)
{
    v00000000 = std::make_unique<Heap>(membase, 0x00000000u, 0x40000000u, 4096);
    v40000000 = std::make_unique<Heap>(membase, 0x40000000u, 0x30000000u, 64 * 1024);
    const uint32_t views[] = {0xA0000000u, 0xC0000000u};
    MapPhysicalMemory(membase, views, 2, kPhysicalSize);
    std::fill(frames.begin(), frames.end(), 0u);
    vA0000000 = std::make_unique<Heap>(membase, 0xA0000000u, kPhysicalSize, 64 * 1024, Heap::Kind::SharedPhysical);
    vC0000000 = std::make_unique<Heap>(membase, 0xC0000000u, kPhysicalSize, 16 * 1024 * 1024, Heap::Kind::SharedPhysical);
    vE0000000 = std::make_unique<Heap>(membase, 0xE0000000u, 0x1FD00000u, 4096, Heap::Kind::Physical, 0x1000);
    // The first 64 KB stay reserved and inaccessible, so null-pointer accesses fault (Xenia).
    v00000000->AllocFixed(0, 0x10000, mem::kReserve, page::kNoAccess);
}

Heap* LookupHeap(uint32_t address)
{
    for (Heap* heap : {v00000000.get(), v40000000.get(), vA0000000.get(), vC0000000.get(), vE0000000.get()})
        if (heap && heap->Contains(address))
            return heap;
    return nullptr;
}

bool PhysicalAddress(uint32_t address, uint32_t& physical)
{
    if (address < 0xA0000000u || (address >= 0xE0000000u && address - 0xE0000000u >= 0x1FD00000u))
        return false;
    physical = address >= 0xE0000000u ? address - 0xE0000000u + 0x1000 : address & (kPhysicalSize - 1);
    return true;
}

bool PhysicalToGuest(uint32_t physical, uint32_t& address)
{
    std::lock_guard guard(lock);
    if (physical >= kPhysicalSize || frames[physical / kFrame] == 0)
        return false;
    const uint32_t view = frames[physical / kFrame];
    address = view == 0xE0000000u ? view + physical - 0x1000 : view + physical;
    return true;
}

Heap* VirtualHeap(uint32_t pageSize) { return pageSize <= 4096 ? v00000000.get() : v40000000.get(); }

Heap* PhysicalHeap(uint32_t pageSize)
{
    return pageSize <= 4096 ? vE0000000.get() : pageSize <= 64 * 1024 ? vA0000000.get() : vC0000000.get();
}

}  // namespace runtime
