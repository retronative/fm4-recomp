// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
//
// The guest front buffer at a swap, as the texture fetch constant VdSwap wrote to
// SHADER_CONSTANT_FETCH_00 (kernel/imports_video.cpp VdSwap) describes it. Field layout: Xenia
// xenia-canary 67f16c4e315f src/xenia/gpu/xenos.h:1206-1300 xe_gpu_texture_fetch_t (dword 0: pitch
// bits 22-30 in units of 32 texels, tiled bit 31; dword 1: format bits 0-5, endianness 6-7,
// base_address 12-31 (>> 12); dword 3: swizzle bits 1-12, 3 bits per component, 0-3 = X-W, 4 zero,
// 5 one). The accepted shapes and the red/blue exchange are ForzaRecomp's present path
// (forzarecomp runtime/main.cpp:209-236, GPL-3.0-or-later, same owner), reviewed here.
//
// TiledOffset and Detile32 are taken from ForzaRecomp runtime/gpu/texture.h:18-42
// (GPL-3.0-or-later, same owner; the formula is Xenia's texture_util TiledOffset2DOuter/Inner,
// BSD-3-Clause), NOT YET REVIEWED here: the review must check them against Xenia 67f16c4
// texture_util.cc (not in the local Xenia copy, which holds gpu/ headers only) and add
// tests whose values come from it. The draft's test checks only formula-independent properties.
// FM4 Recomp changes: namespace runtime::video; the decode is a pure function returning a reason
// instead of stopping; Detile32 takes the source size and refuses a short buffer.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace runtime::video {

struct FrontBuffer
{
    bool ok = false;
    std::string error;
    uint32_t width = 0, height = 0, pitch = 0;  // pitch in texels, a multiple of 32
    uint32_t format = 0;                         // 54: k_2_10_10_10_AS_16_16_16_16 (6: k_8_8_8_8, not presented yet)
    uint32_t physical = 0;                       // base address (physical)
    bool swapRedBlue = false;                    // swizzle ZYXW / ZYX1: the stored word has B in bits 0-9
};

// `fetch`: the six fetch-constant dwords (host order, as read from registers 4800-4805);
// `physical`, `width`, `height`: the XE_SWAP packet's words.
inline FrontBuffer DecodeFrontBuffer(const uint32_t fetch[6], uint32_t physical, uint32_t width, uint32_t height)
{
    FrontBuffer f;
    f.width = width, f.height = height;
    f.pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
    f.format = fetch[1] & 0x3F;
    f.physical = fetch[1] & 0xFFFFF000u;
    const bool tiled = (fetch[0] >> 31) != 0;
    const uint32_t endian = (fetch[1] >> 6) & 3, swizzle = (fetch[3] >> 1) & 0xFFF;
    // XYZW 0x688 (0,1,2,3), XYZ1 0xA88 (0,1,2,5); ZYXW 0x60A (2,1,0,3), ZYX1 0xA0A (2,1,0,5).
    const bool straight = swizzle == 0x688 || swizzle == 0xA88, swapped = swizzle == 0x60A || swizzle == 0xA0A;
    if (f.format != 54)
        f.error = "front buffer format " + std::to_string(f.format) + " is not presented yet (only 54, k_2_10_10_10_AS_16_16_16_16)";
    else if (!tiled || endian != 0)
        f.error = "a linear or byte-swapped front buffer (endian " + std::to_string(endian) + ") is not presented yet";
    else if (!straight && !swapped)
        f.error = "front buffer swizzle " + std::to_string(swizzle) + " is not presented yet";
    else if (width == 0 || height == 0 || f.pitch < width)
        f.error = "front buffer pitch " + std::to_string(f.pitch) + " is smaller than its width " + std::to_string(width);
    else if (f.physical != (physical & 0xFFFFF000u))
        f.error = "the fetch constant's base and the swap packet's front buffer differ";
    else
        f.ok = true, f.swapRedBlue = swapped;
    return f;
}

// The byte offset of texel (x, y) in a tiled 2D surface `pitch` texels wide (a multiple of 32),
// 2^log2Bytes bytes per texel (ForzaRecomp texture.h:18-31, see the header comment).
constexpr uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bytes)
{
    const uint32_t macroY = ((y >> 5) * (pitch >> 5)) << (log2Bytes + 7);
    const uint32_t microY = ((y & 6) << 2) << log2Bytes;
    const uint32_t rowBase = macroY + ((microY & ~0xFu) << 1) + (microY & 0xF) + ((y & 8) << (3 + log2Bytes)) + ((y & 1) << 4);
    const uint32_t macroX = (x >> 5) << (log2Bytes + 7);
    const uint32_t microX = (x & 7) << log2Bytes;
    const uint32_t offset = rowBase + macroX + ((microX & ~0xFu) << 1) + (microX & 0xF);
    return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// The bytes a tiled 32-bit surface occupies: padded to 32x32 tiles.
constexpr size_t TiledSize32(uint32_t pitch, uint32_t height) { return size_t(pitch) * ((height + 31) & ~31u) * 4; }

// Tiled 32-bit surface -> linear (width x height, row-major), texels as the GPU reads them (endian 0).
// False (nothing copied) when the source is shorter than the padded surface.
inline bool Detile32(const uint8_t* tiled, size_t tiledBytes, uint32_t* linear, uint32_t width, uint32_t height, uint32_t pitch)
{
    if (tiledBytes < TiledSize32(pitch, height))
        return false;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            std::memcpy(&linear[size_t(y) * width + x], tiled + TiledOffset(x, y, pitch, 2), 4);
    return true;
}

// ZYXW -> XYZW for a 2_10_10_10 word: exchanges bits 0-9 and 20-29 (ForzaRecomp main.cpp:234-235).
constexpr uint32_t SwapRedBlue1010102(uint32_t p) { return (p & 0xC00FFC00u) | (p & 0x3FFu) << 20 | (p >> 20 & 0x3FFu); }

}  // namespace runtime::video
