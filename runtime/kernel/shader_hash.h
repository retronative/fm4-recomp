// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// The shader hash every record of a shader shares: the census file names (survey/shader_export.py,
// tests/shader_census/census.cpp ShaderHash), the CP trace lines and FM4_SHADER_DUMP names
// (kernel/cp_trace.cpp), and the renderer's shader cache (video/shader_cache.h). ReCarbon's recipe:
// FNV-1a-64, seed cbf29ce484222325 ^ type (vertex 0, pixel 1), one step per 32-bit word loaded
// little-endian from the guest's big-endian bytes (= the host-order word byte-swapped), prime
// 100000001b3. Moved here from cp_trace.cpp (unchanged) so that runtime/video/ and its CPU tests
// use the one definition without linking the kernel.
#pragma once
#include <cstdint>

namespace runtime::gpu {

// `words`: the microcode as the guest's 32-bit values (host order, i.e. already byte-swapped from memory).
inline uint64_t ShaderHash(uint32_t type, const uint32_t* words, uint32_t count)
{
    uint64_t hash = 0xcbf29ce484222325ull ^ type;
    for (uint32_t i = 0; i < count; i++)
        hash = (hash ^ __builtin_bswap32(words[i])) * 0x100000001b3ull;  // a little-endian load of big-endian bytes
    return hash;
}

}  // namespace runtime::gpu
