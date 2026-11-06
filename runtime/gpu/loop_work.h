// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// The per-draw loop-work guard, after ForzaRecomp (forzarecomp-recarbon) runtime/gpu/render.cpp:1314-1349
// (GPL-3.0-or-later, same owner), reviewed and re-tested here. It was added after a GPU
// firmware lockup once loop shaders were enabled (ForzaRecomp): the product of the
// counts of every loop a shader stage reads (an upper bound: sequential loops count as nested)
// times the draw's invocations must stay within 2^26, else the draw stops before anything reaches
// the GPU (fail closed). A 32x32 draw of FM4's 64x64-loop shader is 4.2 M; the same shader full
// screen would be ~3.8 G.
//
// A loop's count is the low byte of its SHADER_CONSTANT_LOOP register (render.cpp:1326; ReCarbon
// runtime/gpu/xenos/xenos.h:1088 LoopConstant, from Xenia: count bits 0-7). A count of 0 counts as 1. A pixel shader's
// invocations are the window scissor's area: PA_SC_WINDOW_SCISSOR_TL/BR x in bits 0-14, y in bits
// 16-30 (render.cpp:1334-1335), width clamped to the surface pitch (RB_SURFACE_INFO bits 0-13) and
// height to 8192 rows; a vertex shader's are its index count.
// FM4 Recomp changes: pure functions; the products are overflow-checked (ForzaRecomp multiplied
// unchecked, so e.g. 2^26 x 2^38 wrapped to 0 and passed); a stage that reads no loop is not bounded
// (as in ForzaRecomp, which skipped it).
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>

namespace runtime::gpu {

constexpr uint64_t kLoopWorkBound = uint64_t(1) << 26;

inline uint64_t PixelInvocations(uint32_t windowScissorTl, uint32_t windowScissorBr, uint32_t surfaceInfo)
{
    const int64_t w = int64_t(windowScissorBr & 0x7FFF) - int64_t(windowScissorTl & 0x7FFF);
    const int64_t h = int64_t((windowScissorBr >> 16) & 0x7FFF) - int64_t((windowScissorTl >> 16) & 0x7FFF);
    const uint64_t width = std::min<uint64_t>(uint64_t(std::max<int64_t>(w, 0)), surfaceInfo & 0x3FFF);
    const uint64_t height = std::min<uint64_t>(uint64_t(std::max<int64_t>(h, 0)), 8192);
    return width * height;
}

// `loopConstants`: the SHADER_CONSTANT_LOOP register value of each loop the stage reads.
inline bool LoopWorkAllowed(std::span<const uint32_t> loopConstants, uint64_t invocations)
{
    if (loopConstants.empty())
        return true;
    uint64_t work = 1;
    for (const uint32_t loop : loopConstants)
    {
        work *= std::max<uint32_t>(loop & 0xFF, 1);  // at most 255 x 2^26: no overflow
        if (work > kLoopWorkBound)
            return false;
    }
    return invocations <= kLoopWorkBound / work;  // work x invocations <= bound, without the product
}

}  // namespace runtime::gpu
