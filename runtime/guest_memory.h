// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/guest_memory.h (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: M3: MapPhysicalMemory (ForzaRecomp guest_memory.h); M4 unit 7:
// SetGuestWriteFaultHook (the write watch's entry, not installed yet).
//
// The guest's 4 GB address space: reserved inaccessible, with only the ranges the runtime commits
// readable and writable. A guest access anywhere else stops the run with the guest address.
#pragma once
#include <cstddef>
#include <cstdint>

namespace runtime {

// Names what a faulting guest address is (e.g. a data-import guard), or returns null.
using DescribeAddress = const char* (*)(uint32_t guest);

uint8_t* ReserveGuestMemory();
void CommitGuestMemory(uint8_t* base, uint64_t guest, uint64_t size);
// SIGSEGV and SIGBUS (macOS reports PROT_NONE accesses as SIGBUS) inside the guest range stop the
// run; any other fault keeps its default action.
void InstallGuestFaultHandler(uint8_t* base, DescribeAddress describe);
// The fault handler runs on an alternate stack, which is per thread: every thread that runs guest
// code calls this first, so a fault on an exhausted host stack is still reported.
void UseFaultStackOnThisThread();
// Maps one shared object of `size` bytes (the guest's physical memory) at each guest address in
// `views`, so the views alias: a write through one is seen through the others. No access until
// a heap grants it.
void MapPhysicalMemory(uint8_t* base, const uint32_t* views, size_t count, uint32_t size);
void ProtectGuestMemoryReadOnly(uint8_t* base, uint64_t guest, uint64_t size);

// The write watch's question (M4 unit 7, runtime/video/texture_cache.h rule 3): a fault at guest address
// `guest` inside the guest range. True: the hook owned the page (a watched page made read-only by the
// watch), recorded the write and made the page writable again; the handler returns and the store re-runs.
// False: not the hook's page; the fault takes the existing path unchanged (a data-import guard, a stop-5
// read-only page, an unmapped address: GuestFault with its name). macOS reports no read/write bit, so the
// hook decides by its own table (a watched page is readable, so any fault on it is a write). It runs on
// the faulting thread inside the signal handler: atomics and mprotect only, no allocation, no lock.
// Null (the default, and the renderer's first version: texture_upload.h kTextureValidity) = no hook.
using WriteFaultHook = bool (*)(uint32_t guest);
void SetGuestWriteFaultHook(WriteFaultHook hook);

inline void StoreBigEndian32(uint8_t* base, uint32_t guest, uint32_t value)
{
    const uint32_t swapped = __builtin_bswap32(value);
    __builtin_memcpy(base + guest, &swapped, 4);
}

}  // namespace runtime
