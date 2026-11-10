// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// FM4 Recomp: GPU-resident resolves (FM4_RESOLVE_RESIDENT=1, opt-in; needs FM4_RESOLVE_DEFER=1). A converting colour resolve
// (edram_vk_plan.h ResolveConvertToGuest's formats) is also converted on the GPU into a 1x R32_UINT image (edram_vk.cpp
// kResolveResident: ResolveConvertPixel's arithmetic per pixel, before the endian swap and the tiling) and copied into every
// host texture image whose fetch constant reads exactly those words: the same base, tiled, the resolve's pitch and endian, one
// level, a 32 bpp format whose host texel is the guest word (k_8_8_8_8 6 / 50, k_2_10_10_10 7 / 54). A draw that samples such
// an image needs no write-back first (no "resolve consumer" flush). The guest memory write-back is unchanged: the staging copy
// and edram_vk.cpp Retired at the next confirmed retire (fences, interrupts, MEM_WRITE and the other CPU sync points still
// flush). This is ReCarbon's fused resolve -> texture path (runtime/gpu/renderer.cpp, s_resolveToTexture), not
// its shared-memory model.
//
// Texel mapping (VERIFIED by tests/runtime/test_resolve_resident.cpp for the traced resolve): ResolveConvertToGuest stores
// rectangle pixel (x, y) at TiledOffset(destOffsetX + x, destOffsetY + y, pitch) from destBase = copyDestBase +
// TiledOffset(x0 & ~31, y0 & ~31, pitch) (edram.cpp DecodeResolve), i.e. texel (x0 + x, y0 + y) of the texture at copyDestBase.
//
// HYPOTHESIS (as FM4_RESOLVE_DEFER's readback rule): the guest CPU does not write a resolve's destination between the resolve
// and the next confirmed retire; at that retire the image's content is taken as the written-back guest bytes' (its reuse
// hash is recomputed from guest memory). FM4_RESOLVE_RESIDENT_CHECK=1 compares the GPU words with the CPU conversion of
// every resident resolve at its retire.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::video {

bool ResolveResident();       // FM4_RESOLVE_RESIDENT=1 and FM4_RESOLVE_DEFER=1 (logged once)
bool ResolveResidentCheck();  // FM4_RESOLVE_RESIDENT_CHECK=1 (diagnostic: one CPU conversion per resident resolve)
// FM4 Recomp (opt-in FM4_RESOLVE_GPU_WORDS=1, needs FM4_RESOLVE_RESIDENT=1; off while the check runs): a resolve converted on
// the GPU is written back to guest memory from those words (copied to the staging buffer instead of its samples), tiled and
// endian-swapped by edram_vk_plan.h ResolveWordsToGuest: no per-sample float conversion on the CPU, no sample dump pass.
bool ResolveGpuWords();
// FM4 Recomp (opt-in FM4_RESOLVE_RESIDENT_K88=1): k_8_8 (16 bpp) resolves are converted on the GPU too (ResidentConvertible).
bool ResolveResidentK88();

// A recorded resolve, as the texture unit sees it. `converted`: the GPU words exist (the image the EDRAM unit returns).
struct ResidentResolve
{
    bool converted = false;
    uint32_t extentStart = 0, extentLength = 0;  // the guest range the write-back will store (ResolveInfo.destExtent*)
    uint32_t textureBase = 0;                    // RB_COPY_DEST_BASE (physical)
    uint32_t destFormat = 0, destPitch = 0, destEndian = 0;
    uint32_t x0 = 0, y0 = 0, width = 0, height = 0;  // the texel rectangle at textureBase
    uint32_t bytesLog2 = 2;  // FM4 Recomp: 2 for 32 bpp words, 1 for k_8_8's 16 bpp words
};

// The converting resolves the GPU path implements: storage 0 (8_8_8_8), 2 (2_10_10_10), 3 (2_10_10_10_FLOAT, 7e3) into
// texture format 6, 7, 50 or 54 (32 bpp).
// FM4 Recomp: also storage 0 into k_8_8 (10), 16 bpp (r | g << 8; FM4's 512x512 k_8_8 chain, about 2.75
// resolve-consumer waits a race frame).
inline bool ResidentConvertible(uint32_t storage, uint32_t destFormat, uint32_t destBytesLog2)
{
    return ((storage == 0 || storage == 2 || storage == 3) && destBytesLog2 == 2 && (destFormat == 6 || destFormat == 7 || destFormat == 50 || destFormat == 54)) ||
           (storage == 0 && destBytesLog2 == 1 && destFormat == 10);
}

// Texture formats whose host texel is the resolve format's guest word (texture_info.cpp: 6 and 50 R8G8B8A8, 7 and 54
// A2B10G10R10; the component mapping is the view's, the same for an upload and a copy).
inline bool ResidentFormatPair(uint32_t destFormat, uint32_t textureFormat)
{
    const auto group = [](uint32_t f) { return f == 6 || f == 50 ? 1 : f == 7 || f == 54 ? 2 : f == 10 ? 3 : 0; };
    return group(destFormat) != 0 && group(destFormat) == group(textureFormat);
}

// Whether a texture image (its key fields and level count) takes the resolve's words verbatim, and at which texel (`x`, `y`)
// the rectangle's corner lands. A resolve whose RB_COPY_DEST_BASE lies inside a tiled 32 bpp texture (FM4's race HDR bands:
// the base moved down by whole tile rows, the rectangle at 0,0) starts at the 32x32 tile its base
// delta names: GetTiledOffset2D (texture_util.cc:443-458; texture_untile.h TiledOffset) of a 32-aligned (X, Y) is
// ((X >> 5) + (Y >> 5) * (pitch >> 5)) << 12 at 4 bytes a texel, and for a 32-aligned corner it adds to the offset of
// (x, y) within (x, y < 32 cannot carry past bit 9 of the pre-swizzle offset), so a delta that is a multiple of 4096 is the
// corner (X, Y); any other delta is refused.
struct ResidentTextureShape
{
    uint32_t base = 0, width = 0, height = 0, pitch = 0, format = 0, endian = 0, levels = 0, dimension = 0, depth = 0;
    bool tiled = false, stacked = false, packedMips = false;
};
inline bool ResidentTextureMatches(const ResidentTextureShape& t, const ResidentResolve& r, std::string* why = nullptr, uint32_t* x = nullptr,
                                   uint32_t* y = nullptr)
{
    uint32_t cornerX = 0, cornerY = 0;
    const char* reason = !r.converted                              ? "the resolve has no GPU words"
                         : t.dimension != 1 || t.depth != 1 || t.stacked ? "not a plain 2D texture"
                         : t.levels != 1 || t.packedMips           ? "more than one level"
                         : !t.tiled                                ? "linear"
                         : t.pitch != r.destPitch || t.pitch == 0 || t.pitch % 32 != 0 ? "another pitch"
                         : r.textureBase < t.base || (r.textureBase - t.base) % 4096 != 0 ? "another base"
                         : r.bytesLog2 != 2 && r.textureBase != t.base                    ? "another base"
                                                                                           : nullptr;
    if (reason == nullptr)
    {
        const uint32_t macro = (r.textureBase - t.base) >> 12, tilesPerRow = t.pitch >> 5;
        cornerX = macro % tilesPerRow * 32, cornerY = macro / tilesPerRow * 32;
        reason = t.endian != r.destEndian                ? "another endian"
                 : !ResidentFormatPair(r.destFormat, t.format) ? "a format whose texel is not the resolve's word"
                 : uint64_t(cornerX) + r.x0 + r.width > t.width || uint64_t(cornerY) + r.y0 + r.height > t.height ? "the rectangle exceeds the texture"
                                                                                                                  : nullptr;
    }
    if (reason != nullptr && why != nullptr)
        *why = reason;
    if (x != nullptr)
        *x = cornerX + r.x0;
    if (y != nullptr)
        *y = cornerY + r.y0;
    return reason == nullptr;
}

// FM4 Recomp: a mipped texture whose levels FM4 fills one resolve each (the 256x256 k_8_8_8_8 at
// 1C2E3000, then 128, 64 and 32 at 1C323000, 1C333000, 1C337000; ~4.75 consumer waits a race frame). Each unpacked level is
// matched as a one-level texture at its own address, size and pitch (texture_info.h MipLevel); a packed level (its texels at an
// offset inside a shared tail tile) is never matched. Returns the level the resolve's words go to (corner in `x`, `y`), or -1
// with the reason level 0 gave (a single-level, unpacked texture: exactly ResidentTextureMatches).
struct ResidentLevel
{
    uint32_t address = 0, width = 0, height = 0, pitch = 0;
    bool packed = false;
};
inline int ResidentLevelFor(const ResidentTextureShape& t, const std::vector<ResidentLevel>& levels, const ResidentResolve& r, std::string* why = nullptr,
                            uint32_t* x = nullptr, uint32_t* y = nullptr)
{
    if ((t.levels == 1 && !t.packedMips) || levels.empty())
        return ResidentTextureMatches(t, r, why, x, y) ? 0 : -1;
    std::string first;
    for (size_t level = 0; level < levels.size(); level++)
    {
        const ResidentLevel& l = levels[level];
        std::string reason = "a packed mip level";
        if (!l.packed)
        {
            ResidentTextureShape one = t;
            one.base = l.address, one.width = l.width, one.height = l.height, one.pitch = l.pitch, one.levels = 1, one.packedMips = false;
            if (ResidentTextureMatches(one, r, &reason, x, y))
                return int(level);
        }
        if (level == 0)
            first = reason;
    }
    if (why != nullptr)
        *why = "no level takes it (level 0: " + first + ")";
    return -1;
}

}  // namespace runtime::video
