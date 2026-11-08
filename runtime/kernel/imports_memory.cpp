// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/kernel/imports_memory.cpp (GPL-3.0-or-later, same owner), reviewed and
// re-tested here (tests/run_runtime_tests.sh). FM4 Recomp changes: none.
//
// Memory imports (M3-T04/T05). REFERENCE_DERIVED from Xenia (xenia-canary 67f16c4e315f:
// kernel/xboxkrnl/xboxkrnl_memory.cc), re-derived over virtual_memory.h. Pages are zero whenever
// they become committed (fresh, released and decommitted pages are cleared), which covers
// Xenia's zero-unless-MEM_NOZERO rule for every page that was not committed before.
#include "imports.h"
#include "arena.h"
#include "fibers.h"
#include "virtual_memory.h"
#include <mutex>
#include <set>

namespace {

constexpr uint32_t kSuccess = 0, kInvalidParameter = 0xC000000D, kNoMemory = 0xC0000017, kAccessDenied = 0xC0000022;

bool IsVirtualHeap(const runtime::Heap* heap)
{
    return heap && (heap == runtime::VirtualHeap(4096) || heap == runtime::VirtualHeap(64 * 1024));
}

}  // namespace

GUEST_IMPORT(NtAllocateVirtualMemory, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc NtAllocateVirtualMemory")
{
    using namespace runtime;
    const uint32_t basePointer = ctx.r3.u32, sizePointer = ctx.r4.u32, type = ctx.r5.u32, protect = ctx.r6.u32;
    if (ctx.r7.u32 != 0)
        Stop(StopCode::Unimplemented, "NtAllocateVirtualMemory: DebugMemory (devkit memory) is not implemented");
    if (!basePointer || !sizePointer || !Load32(base, sizePointer) || !(type & (mem::kCommit | mem::kReset | mem::kReserve)) ||
        ((type & mem::kReset) && (type & ~mem::kReset)))
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    if (type & mem::kReset)
        Stop(StopCode::Unimplemented, "NtAllocateVirtualMemory: MEM_RESET is not implemented (Xenia neither)");

    const uint32_t requested = Load32(base, basePointer);
    Heap* heap = requested ? LookupHeap(requested) : nullptr;
    if (requested && !IsVirtualHeap(heap))
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    const uint32_t pageSize = requested ? heap->pageSize() : (type & mem::kLargePages) ? 64 * 1024 : 4096;
    const uint32_t adjustedBase = requested - requested % pageSize;
    uint32_t size = Load32(base, sizePointer);
    if (int32_t(size) < 0)
        size = uint32_t(-int32_t(size));  // some titles pass negative sizes (Xenia)
    const uint32_t round = adjustedBase ? pageSize : 64 * 1024;
    size = (size + round - 1) / round * round;

    uint32_t address = 0;
    if (adjustedBase)
    {
        if (heap->pageSize() != pageSize)
        {
            ctx.r3.u64 = kAccessDenied;
            return;
        }
        if (heap->AllocFixed(adjustedBase, size, type & (mem::kReserve | mem::kCommit), protect))
            address = adjustedBase;
    }
    else
        address = VirtualHeap(pageSize)->Alloc(size, pageSize, type & (mem::kReserve | mem::kCommit), protect,
                                               (type & mem::kTopDown) != 0);
    if (!address)
    {
        ctx.r3.u64 = kNoMemory;
        return;
    }
    Store32(base, basePointer, address);
    Store32(base, sizePointer, size);
    ctx.r3.u64 = kSuccess;
}

GUEST_IMPORT(MmAllocatePhysicalMemoryEx, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc xeMmAllocatePhysicalMemoryEx")
{
    // (flags, size, protect | page-size flag, lowest and highest physical address, alignment):
    // reserved and committed pages of the physical view for the page size (MEM_LARGE_PAGES 64 KB
    // at A0000000, MEM_16MB_PAGES 16 MB at C0000000, else 4 KB at E0000000, which starts at
    // physical 0x1000; each allocation takes physical frames no other view holds;
    // virtual_memory.h), highest first; returns the guest address, or 0 when there is no room or
    // the protection is neither read-only nor read-write. Only a range covering the whole view is
    // implemented; a narrower one stops. The flags (0 from titles, 1 and 2 from D3D per Xenia)
    // have no known effect here, so any other value stops.
    using namespace runtime;
    const uint32_t flags = ctx.r3.u32, size = ctx.r4.u32, protectBits = ctx.r5.u32;
    const uint32_t lowest = ctx.r6.u32, highest = ctx.r7.u32, alignment = ctx.r8.u32;
    if (flags != 0)
        Stop(StopCode::Unimplemented, "MmAllocatePhysicalMemoryEx: flags %X are not modelled", flags);
    if (!(protectBits & (page::kReadOnly | page::kReadWrite)))
    {
        ctx.r3.u64 = 0;
        return;
    }
    const uint32_t pageSize = (protectBits & mem::kLargePages) ? 64 * 1024 : (protectBits & mem::k16MbPages) ? 16 * 1024 * 1024 : 4096;
    Heap* heap = PhysicalHeap(pageSize);
    const uint32_t physicalBase = pageSize == 4096 ? 0x1000 : 0;  // Xenia PhysicalHeap::GetPhysicalAddress
    if (lowest > physicalBase || highest < physicalBase + (heap->size() - 1))
        Stop(StopCode::Unimplemented, "MmAllocatePhysicalMemoryEx: a physical range %08X-%08X narrower than the view is not "
             "implemented", lowest, highest);
    const uint64_t rounded = (uint64_t(size) + pageSize - 1) / pageSize * pageSize;
    const uint64_t aligned = (uint64_t(alignment) + pageSize - 1) / pageSize * pageSize;
    if (rounded == 0 || rounded > heap->size() || aligned > heap->size())
    {
        ctx.r3.u64 = 0;
        return;
    }
    const uint32_t protect = protectBits & ~(mem::kLargePages | mem::k16MbPages);
    ctx.r3.u64 = heap->Alloc(uint32_t(rounded), uint32_t(aligned), mem::kReserve | mem::kCommit, protect, true);
}

GUEST_IMPORT(MmSetAddressProtect, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc MmSetAddressProtect")
{
    // (address, size, protect), no result: the protection of committed pages. Exactly one of
    // no-access, read-only, read-write, execute-read and execute-read-write is expected. With none
    // of them Xenia leaves the pages as they are (it notes that many titles pass such values),
    // which is followed; more than one, or pages that are not committed in one heap, stop.
    using namespace runtime;
    const uint32_t address = ctx.r3.u32, size = ctx.r4.u32, protect = ctx.r5.u32;
    const uint32_t kinds = protect & (page::kNoAccess | page::kReadOnly | page::kReadWrite | page::kExecuteRead | page::kExecuteReadWrite);
    if (kinds == 0)
        return;
    if (kinds & (kinds - 1))
        Stop(StopCode::Unimplemented, "MmSetAddressProtect(%08X, %X, %X): more than one access kind", address, size, protect);
    Heap* heap = LookupHeap(address);
    if (heap == nullptr || !heap->Protect(address, size, protect, nullptr))
        Stop(StopCode::Unimplemented, "MmSetAddressProtect(%08X, %X, %X): the range is not committed in one heap", address, size, protect);
}

GUEST_IMPORT(MmQueryAddressProtect, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc MmQueryAddressProtect")
{
    // (address): the protection of a committed page, 0 for a reserved or free one. The heaps keep
    // the XDK flags the title set, so they are returned as set (Xenia converts through its own
    // flags, which turns the execute kinds into plain read or read-write: not reproduced,
    // INFERRED). Guest stacks (outside the heaps here; Xenia allocates them read/write with
    // no-access guard pages) answer read/write for their pages and 0 for guards and the unused
    // stack range. Any other address outside the heaps (an image) stops.
    using namespace runtime;
    const uint32_t address = ctx.r3.u32;
    if (address >= kStacksBase && address < kStacksEnd)
    {
        ctx.r3.u64 = InGuestStack(address) ? page::kReadWrite : 0;
        return;
    }
    const Heap* heap = LookupHeap(address);
    RegionInfo info{};
    if (heap == nullptr || !heap->Query(address, info))
        Stop(StopCode::Unimplemented, "MmQueryAddressProtect(%08X): not in a heap", address);
    ctx.r3.u64 = info.protect;  // Heap::Query reports 0 for a page that is not committed
}

GUEST_IMPORT(NtFreeVirtualMemory, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc NtFreeVirtualMemory")
{
    // (base*, size*, free type, debug memory): MEM_DECOMMIT of whole pages (the size rounds up to
    // the heap's page), or MEM_RELEASE of the allocation at its base; the base and the size freed
    // are written back. A null base is STATUS_MEMORY_NOT_ALLOCATED; a physical heap is an invalid
    // parameter; a failed free is STATUS_UNSUCCESSFUL. Not modelled, so they stop: devkit memory,
    // other free types, a base inside a page (NT rounds it; Xenia does not), a decommit of size 0
    // (the rest of the region in NT) and a release with a size (an invalid parameter in NT).
    using namespace runtime;
    const uint32_t basePointer = ctx.r3.u32, sizePointer = ctx.r4.u32, type = ctx.r5.u32;
    if (ctx.r6.u32 != 0)
        Stop(StopCode::Unimplemented, "NtFreeVirtualMemory: DebugMemory (devkit memory) is not implemented");
    const uint32_t address = Load32(base, basePointer);
    uint32_t size = Load32(base, sizePointer);
    if (address == 0)
    {
        ctx.r3.u64 = 0xC00000A0;  // STATUS_MEMORY_NOT_ALLOCATED
        return;
    }
    Heap* heap = LookupHeap(address);
    if (heap == nullptr)
        Stop(StopCode::Unimplemented, "NtFreeVirtualMemory(%08X): not in a heap", address);
    if (!IsVirtualHeap(heap))
    {
        ctx.r3.u64 = kInvalidParameter;
        return;
    }
    if ((type != mem::kDecommit && type != mem::kRelease) || address % heap->pageSize() != 0 ||
        (type == mem::kDecommit && size == 0) || (type == mem::kRelease && size != 0))
        Stop(StopCode::Unimplemented, "NtFreeVirtualMemory(%08X, %X, type %X): not modelled", address, size, type);
    bool freed;
    if (type == mem::kDecommit)
    {
        size = uint32_t((uint64_t(size) + heap->pageSize() - 1) / heap->pageSize() * heap->pageSize());
        freed = heap->Decommit(address, size);
    }
    else
        freed = heap->Release(address, &size);
    if (!freed)
    {
        ctx.r3.u64 = 0xC0000001;  // STATUS_UNSUCCESSFUL
        return;
    }
    Store32(base, basePointer, address);
    Store32(base, sizePointer, size);
    ctx.r3.u64 = kSuccess;
}

GUEST_IMPORT(MmGetPhysicalAddress, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc MmGetPhysicalAddress, memory.cc PhysicalHeap::GetPhysicalAddress")
{
    // (address): its physical address: the offset in the A0000000 or C0000000 view, or the offset
    // in the E0000000 view plus 0x1000. Other addresses (virtual memory, images) stop: Xenia
    // returns 0 for them with an assertion, which would be a fabricated answer.
    uint32_t physical = 0;
    if (!runtime::PhysicalAddress(ctx.r3.u32, physical))
        Stop(StopCode::Unimplemented, "MmGetPhysicalAddress(%08X): not in a modelled physical view", ctx.r3.u32);
    ctx.r3.u64 = physical;
}

GUEST_IMPORT(MmFreePhysicalMemory, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc MmFreePhysicalMemory")
{
    // (type, address): releases a physical allocation at its base. An address that is no
    // physical allocation's base stops (Xenia ignores the failure). The type is not used (Xenia).
    using namespace runtime;
    const uint32_t address = ctx.r4.u32;
    Heap* heap = LookupHeap(address);
    const bool physical = heap == PhysicalHeap(4096) || heap == PhysicalHeap(64 * 1024) || heap == PhysicalHeap(16 * 1024 * 1024);
    if (!physical || !heap->Release(address, nullptr))
        Stop(StopCode::Unimplemented, "MmFreePhysicalMemory(%u, %08X): not the base of a physical allocation", ctx.r3.u32, address);
}

namespace {
std::mutex xamHeapLock;
std::set<uint32_t> xamBlocks;  // blocks XamAlloc handed out (their base addresses)
}  // namespace

GUEST_IMPORT(XamAlloc, "REFERENCE_DERIVED: Xenia xam_info.cc:361-382 XamAllocImpl (system heap, X_ERROR_SUCCESS)")
{
    // (flags, size, block*): a zeroed block of `size` bytes, 0. Xenia allocates from its system heap; here the block
    // is whole pages of the 4 KB virtual heap (a freeable allocation; pages are zeroed when committed, which also
    // satisfies flag 00100000, "no zeroing needed"). The flags' top 4 bits select one of XAM's heaps (Xenia's note); Xenia
    // serves every heap from its one system heap, and so does this (FM4's task blocks use heap 1). Any
    // other flag bit, a zero size or a full heap stop (7).
    const uint32_t flags = ctx.r3.u32, size = ctx.r4.u32, out = ctx.r5.u32;
    if ((flags & ~0xF0100000u) != 0 || size == 0)
        Stop(StopCode::Unimplemented, "XamAlloc(flags %08X, size %u) is not modelled", flags, size);
    const uint32_t block = runtime::VirtualHeap(4096)->Alloc(size, 4096, runtime::mem::kReserve | runtime::mem::kCommit,
                                                             runtime::page::kReadWrite, false);
    if (block == 0)
        Stop(StopCode::Unimplemented, "XamAlloc: no room for %u bytes", size);
    {
        std::lock_guard guard(xamHeapLock);
        xamBlocks.insert(block);
    }
    runtime::Store32(base, out, block);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamFree, "REFERENCE_DERIVED: Xenia xam_info.cc:413-418 (system heap free, X_ERROR_SUCCESS)")
{
    // (block*): frees a block XamAlloc handed out, 0. Anything else stops (7): Xenia's heap would ignore or corrupt.
    const uint32_t block = ctx.r3.u32;
    {
        std::lock_guard guard(xamHeapLock);
        if (xamBlocks.erase(block) == 0)
            Stop(StopCode::Unimplemented, "XamFree(%08X): not a block XamAlloc handed out", block);
    }
    uint32_t released = 0;
    if (!runtime::VirtualHeap(4096)->Release(block, &released))
        Stop(StopCode::Unimplemented, "XamFree(%08X): the heap could not release it", block);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(MmCreateKernelStack, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc:713-728 (a committed read/write stack in the 0x70000000 range; returns its top)")
{
    // (size, r4 = 0): a guest stack (arena.h: committed, zeroed, unmapped guard pages around it, at least `size` bytes
    // rounded up to the page) and its top, one past the highest usable byte, as Xenia's stack_address + stack_size.
    // Xenia asserts r4 is zero (an unknown argument): another value stops here. A zero size stops.
    const uint32_t size = ctx.r3.u32, unknown = ctx.r4.u32;
    if (!size || unknown)
        Stop(StopCode::Unimplemented, "MmCreateKernelStack(size %u, r4 %08X): a zero size or a non-zero second argument", size, unknown);
    ctx.r3.u64 = runtime::AllocateStack(base, size).base;
}

GUEST_IMPORT(MmDeleteKernelStack, "REFERENCE_DERIVED: Xenia xboxkrnl_memory.cc:730-739 (the stack released; SUCCESS, or UNSUCCESSFUL if it is no stack)")
{
    // (stack base = its top, stack end = its lowest usable byte), as DeleteFiber passes them (block +4, +12;
    // the local capture delete_fiber (not distributed)). A fiber running on it is forgotten first (fibers.h); then the stack is released
    // (arena.h: its pages dropped and inaccessible). Not a stack MmCreateKernelStack returned: STATUS_UNSUCCESSFUL.
    const uint32_t top = ctx.r3.u32, limit = ctx.r4.u32;
    runtime::DiscardFiber(top);
    ctx.r3.u64 = runtime::ReleaseStack(base, top, limit) ? 0 : 0xC0000001;  // STATUS_UNSUCCESSFUL
}
