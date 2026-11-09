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

namespace runtime::video {

bool ResolveResident();       // FM4_RESOLVE_RESIDENT=1 and FM4_RESOLVE_DEFER=1 (logged once)
bool ResolveResidentCheck();  // FM4_RESOLVE_RESIDENT_CHECK=1 (diagnostic: one CPU conversion per resident resolve)

// A recorded resolve, as the texture unit sees it. `converted`: the GPU words exist (the image the EDRAM unit returns).
struct ResidentResolve
{
    bool converted = false;
    uint32_t extentStart = 0, extentLength = 0;  // the guest range the write-back will store (ResolveInfo.destExtent*)
    uint32_t textureBase = 0;                    // RB_COPY_DEST_BASE (physical)
    uint32_t destFormat = 0, destPitch = 0, destEndian = 0;
    uint32_t x0 = 0, y0 = 0, width = 0, height = 0;  // the texel rectangle at textureBase
};

// The converting resolves the GPU path implements: storage 0 (8_8_8_8), 2 (2_10_10_10), 3 (2_10_10_10_FLOAT, 7e3) into
// texture format 6, 7, 50 or 54 (32 bpp).
inline bool ResidentConvertible(uint32_t storage, uint32_t destFormat, uint32_t destBytesLog2)
{
    return (storage == 0 || storage == 2 || storage == 3) && destBytesLog2 == 2 && (destFormat == 6 || destFormat == 7 || destFormat == 50 || destFormat == 54);
}

// Texture formats whose host texel is the resolve format's guest word (texture_info.cpp: 6 and 50 R8G8B8A8, 7 and 54
// A2B10G10R10; the component mapping is the view's, the same for an upload and a copy).
inline bool ResidentFormatPair(uint32_t destFormat, uint32_t textureFormat)
{
    const auto group = [](uint32_t f) { return f == 6 || f == 50 ? 1 : f == 7 || f == 54 ? 2 : 0; };
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

}  // namespace runtime::video
