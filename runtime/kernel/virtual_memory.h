// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/virtual_memory.h (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Guest virtual memory (M3-T05): page-table heaps with reserve/commit/release/decommit, query and
// protect, as the kernel's Nt*VirtualMemory and Mm* calls see them. REFERENCE_DERIVED from Xenia
// (xenia-canary 67f16c4e315f: memory.{h,cc} BaseHeap, Memory::Initialize heap map,
// kernel/xboxkrnl/xboxkrnl_memory.cc), re-derived. Differences, by design:
//  - the 64 KB-page heap ends at 0x70000000, where this runtime's stacks and kernel arena begin
//    (Xenia's ends at 0x7F000000 and also holds its stacks), so top-down addresses differ;
//  - the physical views 0xA0000000 (64 KB pages) and 0xC0000000 (16 MB pages) map one shared
//    512 MB physical memory, so they alias as on the console. The 0xE0000000 view (4 KB pages)
//    maps physical memory from 0x1000, a shift 16 KB host pages cannot express, so it has its own
//    host memory: its addresses and physical addresses are exact (E0000000 + x is physical
//    x + 0x1000), but the same physical memory reached through another view is not shared with
//    it. One table of 4 KB physical frames (Xenia's parent physical heap) gives each frame to one
//    view's allocation and grants host access only through views that share its memory, so such a
//    cross-view access faults and stops instead of reading other data;
//  - the host enforces protection per host page (16 KB on Apple silicon), so a 4 KB guest page
//    shares its host page's access with its neighbours.
#pragma once
#include <cstdint>
#include <vector>

namespace runtime {

// Xbox kernel constants (the XDK's MEM_* and PAGE_* values).
namespace mem { constexpr uint32_t kCommit = 0x1000, kReserve = 0x2000, kDecommit = 0x4000, kRelease = 0x8000,
                                   kFree = 0x10000, kPrivate = 0x20000, kReset = 0x80000, kTopDown = 0x100000,
                                   kNoZero = 0x800000, kLargePages = 0x20000000, k16MbPages = 1u << 31; }
namespace page { constexpr uint32_t kNoAccess = 0x01, kReadOnly = 0x02, kReadWrite = 0x04, kWriteCopy = 0x08,
                                    kExecute = 0x10, kExecuteRead = 0x20, kExecuteReadWrite = 0x40,
                                    kExecuteWriteCopy = 0x80, kGuard = 0x100, kNoCache = 0x200, kWriteCombine = 0x400; }

struct RegionInfo { uint32_t base, allocationBase, allocationProtect, size, state, protect; };

class Heap
{
public:
    // A physical view: its first page is physical address `physicalBase`; `shared`: it maps the
    // shared physical memory (A0000000, C0000000), whose host access it mirrors to the other view.
    enum class Kind { Virtual, Physical, SharedPhysical };
    Heap(uint8_t* membase, uint32_t base, uint32_t size, uint32_t pageSize, Kind kind = Kind::Virtual, uint32_t physicalBase = 0);
    uint32_t base() const { return base_; }
    uint32_t size() const { return uint32_t(pages_.size()) * pageSize_; }
    uint32_t pageSize() const { return pageSize_; }
    bool Contains(uint32_t address) const { return address >= base_ && address - base_ < size(); }

    // Reserve and/or commit at `address` (page-aligned, whole pages); false if not possible.
    bool AllocFixed(uint32_t address, uint32_t size, uint32_t type, uint32_t protect);
    // Reserve (and commit) a free run, lowest or highest first; 0 if none.
    uint32_t Alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool topDown);
    bool Release(uint32_t address, uint32_t* releasedSize);  // the whole allocation at its base
    bool Decommit(uint32_t address, uint32_t size);
    bool Protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t* oldProtect);
    bool Query(uint32_t address, RegionInfo& info) const;
    bool IsCommitted(uint32_t address, uint32_t size) const;

private:
    struct Page { uint32_t allocationBase = 0, allocationPages = 0, state = 0, protect = 0, allocationProtect = 0; };
    void SetHostAccess(uint32_t first, uint32_t count);
    void Zero(uint32_t first, uint32_t count);
    bool Claim(uint32_t first, uint32_t count, bool take);  // physical frames of pages [first, first + count)
    uint8_t* membase_;
    uint32_t base_, pageSize_;
    Kind kind_;
    uint32_t physicalBase_;
    std::vector<Page> pages_;
};

// The heap holding a guest address, or null (outside the managed ranges).
Heap* LookupHeap(uint32_t address);

// FM4 Recomp (FM4_WRITE_WATCH, runtime/video/texture_cache.h WriteWatch): the page write watch's side of the heaps. Only the
// shared physical memory (A0000000 and C0000000, one host mapping seen through both views) is watched; the E0000000 view has its
// own memory and is never protected for the watch. Host pages are 16 KB (kWatchHostPage; WatchAvailable is false on any other
// host page size).
constexpr uint32_t kWatchHostPage = 16384;
bool WatchAvailable();
// Whether the heap means the host page holding `physical` to be writable (lock-free; read by the fault hook).
bool PhysicalHostWritable(uint32_t physical);
// Under the heap lock: every 4 KB frame of [physical, physical + size) is owned by the A0000000 or C0000000 view and every host
// page of it is meant writable; then the host pages read-only in both views. False (nothing protected): not watchable.
bool WatchProtect(uint32_t physical, uint32_t size);
// Signal-safe (no lock, mprotect only): the host pages of [physical, physical + size) the heap means writable get read-write
// back in both views. False: an mprotect failed or a page is not meant writable.
bool WatchUnprotect(uint32_t physical, uint32_t size);
// The watch's observers (null: none): `accessChanged` before the heap sets a shared physical host page's access or clears it
// (Zero); `hostWrite` before host code writes guest memory through a system call (BeforeHostWrite), which cannot fault.
using PhysicalRangeObserver = void (*)(uint32_t physical, uint32_t size);
void SetWatchObservers(PhysicalRangeObserver accessChanged, PhysicalRangeObserver hostWrite);
// Before a system call writes `size` bytes at host address `host` (pread into a guest buffer): the watched pages it covers are
// made writable and counted as written. Nothing for memory outside the shared physical views.
void BeforeHostWrite(const uint8_t* host, uint32_t size);
// The general-purpose virtual heap for a page size (4 KB or 64 KB).
Heap* VirtualHeap(uint32_t pageSize);
// The physical heap Xenia uses for a page size (4 KB -> 0xE0000000, 64 KB -> 0xA0000000, 16 MB -> 0xC0000000).
// The 0xE0000000 view is not modelled (see above): callers stop on it.
Heap* PhysicalHeap(uint32_t pageSize);
void InitializeHeaps(uint8_t* membase);

constexpr uint32_t kPhysicalSize = 0x20000000;  // 512 MB
// The physical address of a guest address in a modelled physical view (its offset in the view);
// nullopt-like false for any other address.
bool PhysicalAddress(uint32_t address, uint32_t& physical);
// The guest address through which a physical address is reached: the view whose allocation
// holds its frame; false if no view holds it.
bool PhysicalToGuest(uint32_t physical, uint32_t& address);
// FM4 Recomp (M7): PhysicalToGuest for a whole range under one lock: true only when every frame of [physical, physical + bytes)
// is held by the same view, so the range is contiguous at `address` (false for bytes 0 or any frame elsewhere or free).
bool PhysicalRangeToGuest(uint32_t physical, uint32_t bytes, uint32_t& address);

}  // namespace runtime
