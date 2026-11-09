// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// Taken from ForzaRecomp runtime/layout.h (GPL-3.0-or-later, same owner), reviewed and re-tested here.
// FM4 Recomp changes: M3: the single M2 stack (kStackTop, kStackSize) is gone; guest stacks come from
// kernel/arena.h (0x70000000-0x7E000000, the kernel arena 0x7E000000-0x7F000000, below the guards).
//
// Guest address-space choices of this runtime (not the title's) and the pure lookups built on
// them, kept here so they can be unit-tested (tests/runtime).
#pragma once
#include <cstddef>
#include <cstdint>

namespace runtime {

// Data imports have no documented values yet, so each import slot points at its own guard page,
// which is never mapped: the first access faults and names the import (M2-T04).
constexpr uint32_t kGuardBase = 0x7F000000u;

constexpr uint32_t kGuardEnd = 0x7FC80000u;  // the GPU register page (kernel/graphics.h)

constexpr uint32_t GuardAddress(size_t index, size_t page) { return kGuardBase + uint32_t(index * page); }
constexpr bool GuardsFit(size_t count, size_t page) { return count * page <= kGuardEnd - kGuardBase; }

// The import index whose guard page holds address, or -1 if it is no guard.
constexpr long GuardIndex(uint32_t address, size_t page, size_t count)
{
    if (address < kGuardBase || (address - kGuardBase) / page >= count)
        return -1;
    return long((address - kGuardBase) / page);
}

// An image section, as the survey reports it.
struct Section { uint32_t start, end; bool code; };

// A page of the image stays writable if any data section overlaps it; a page holding only code
// (or nothing) is read-only, so a wild guest write into code stops instead of succeeding.
constexpr bool PageWritable(uint32_t page, size_t size, const Section* sections, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (!sections[i].code && sections[i].start < page + size && page < sections[i].end)
            return true;
    return false;
}

// A possible translated-function address: a word inside the code range.
constexpr bool IsCodeAddress(uint32_t address, uint64_t codeBase, uint64_t codeSize)
{
    return address >= codeBase && address - codeBase < codeSize && (address & 3) == 0;
}

// IEEE CRC-32 (as zlib.crc32): identifies the image the translation was generated from.
inline uint32_t Crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
#if defined(__ARM_FEATURE_CRC32)
    // FM4 Recomp (M7): the ARMv8 CRC32X / CRC32B instructions compute the same reflected polynomial (0xEDB88320) eight bytes
    // (little-endian, first byte lowest) a step; the bitwise loop below cost ~20 ms on each 2.7 MB capture PNG.
    for (; size >= 8; data += 8, size -= 8)
    {
        uint64_t v;
        __builtin_memcpy(&v, data, 8);
        crc = __builtin_arm_crc32d(crc, v);
    }
    for (; size > 0; data++, size--)
        crc = __builtin_arm_crc32b(crc, *data);
    return ~crc;
#endif
    for (size_t i = 0; i < size; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

}  // namespace runtime
