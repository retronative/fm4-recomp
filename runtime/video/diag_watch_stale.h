// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// Diagnostic only (reached from FM4_WRITE_WATCH_VERIFY=1's stop; never a default): the state of every 16 KB host page of a
// texture the verify found changed without a recorded write, so the stop names which side missed the write:
//   watched=1 and the host page read-only in both views: the write bypassed the protection (another mapping or a system call);
//   watched=0 or the page writable, with the page's write sequence <= the image's: the page was made writable without counting a
//   write (a bookkeeping path that unprotects without MarkWritten).
#pragma once
#include "texture_cache.h"
#include <cstdint>
#include <cstdio>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace runtime::video::diag {

// The host protection of one guest address ("rw", "r-", "--", or "?" when the region query fails).
inline const char* HostProtection(const uint8_t* host)
{
    mach_vm_address_t address = mach_vm_address_t(uintptr_t(host));
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, vm_region_info_t(&info), &count, &object) != KERN_SUCCESS ||
        address > mach_vm_address_t(uintptr_t(host)))
        return "?";
    if (info.protection & VM_PROT_WRITE)
        return "rw";
    return (info.protection & VM_PROT_READ) ? "r-" : "--";
}

inline void ReportStalePages(const WriteWatch& w, const uint8_t* base, uint32_t physical, uint32_t size, uint64_t imageSequence, bool hostWritable(uint32_t))
{
    const uint32_t host = w.HostPageSize();
    for (uint32_t h = physical / host * host; h < physical + size; h += host)
        std::fprintf(stderr, "[diag] stale texture host page %08X: watched %d, A0 view %s, C0 view %s, heap writable %d, write sequence %llu (image %llu)\n", h,
                     int(w.IsWatchedHostPage(h)), HostProtection(base + 0xA0000000u + h), HostProtection(base + 0xC0000000u + h), int(hostWritable(h)),
                     (unsigned long long)w.WriteSequence(h, host), (unsigned long long)imageSequence);
}

// A clean bind whose texture was written after its reservation decided "clean": counted, the first few named.
inline void ReportReservationRace(uint32_t physical)
{
    static uint64_t count = 0;
    if (++count <= 8 || (count & (count - 1)) == 0)
        std::fprintf(stderr, "[diag] write watch verify: texture %08X written after its reservation (a guest store racing the draw; %llu so far)\n",
                     physical, (unsigned long long)count);
}

}  // namespace runtime::video::diag
