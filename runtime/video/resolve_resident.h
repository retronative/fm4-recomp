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
// 32 bpp destinations; k_8_8's 16 bpp words too with FM4_RESOLVE_GPU_WORDS16=1 (edram_vk.cpp ResolveGpuWords16, M7 CPU cost).
bool ResolveGpuWords();
// FM4 Recomp (opt-in FM4_RESOLVE_RESIDENT_K88=1): k_8_8 (16 bpp) resolves are converted on the GPU too (ResidentConvertible).
bool ResolveResidentK88();
bool ResolveResidentDepth();  // FM4_RESOLVE_RESIDENT_DEPTH=1 (opt-in): depth resolves as resident words (renderer.cpp)
// FM4 Recomp (M7 CPU cost; opt-in FM4_RESOLVE_RESIDENT_1616=1, needs FM4_RESOLVE_RESIDENT=1): 16_16 (storage 4) resolves into k_16_16
// (25) are converted on the GPU too (ResidentConvertible; edram_vk.cpp kResolveResident XE_SOURCE_UINT), so with FM4_RESOLVE_GPU_WORDS
// their write-back is ResolveWordsToGuest instead of 4 sample unpacks + PackFixed16 per pixel (FM4's 512x512 4x one every race frame).
bool ResolveResident1616();

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
// FM4 Recomp (M7 CPU cost): also storage 4 (16_16) into k_16_16 (25), 32 bpp (the only pairing DecodeResolve admits for either side;
// edram_vk.cpp gates it behind FM4_RESOLVE_RESIDENT_1616).
inline bool ResidentConvertible(uint32_t storage, uint32_t destFormat, uint32_t destBytesLog2)
{
    return ((storage == 0 || storage == 2 || storage == 3) && destBytesLog2 == 2 && (destFormat == 6 || destFormat == 7 || destFormat == 50 || destFormat == 54)) ||
           (storage == 0 && destBytesLog2 == 1 && destFormat == 10) || (storage == 4 && destBytesLog2 == 2 && destFormat == 25);
}

// Texture formats whose host texel is the resolve format's guest word (texture_info.cpp: 6 and 50 R8G8B8A8, 7 and 54
// A2B10G10R10; the component mapping is the view's, the same for an upload and a copy).
// FM4 Recomp (M7 waits): 36 (k_32_FLOAT) with itself: a fast 32_FLOAT resolve's resident words are the source texels (edram_vk.cpp
// residentCopy), read back as R32_SFLOAT without a load transform.
// FM4 Recomp (FM4_RESOLVE_RESIDENT_DEPTH): a depth resolve (dest 22 k_24_8 or 23 k_24_8_FLOAT) read as a raw 32-bit colour format (6,
// 7, 50, 54): the texel is the guest word (an upload is a detile and endian swap only). Not 23 read as 23 (that upload decodes the
// 20e4 depth to a float: a second image would be needed) and never 22 <-> 23 (different depth encodings).
// FM4 Recomp (M7 CPU cost): 25 (k_16_16) with itself: the host texel is R16G16_UNORM or _SNORM (texture_info.cpp HostFormatFor, by the
// fetch's sign), 32 bits whose low half is red, i.e. the resolve's word r | g << 16 before its endian swap, which the upload undoes with
// the same endian (texture_untile.h EndianSwap32 == edram.h GpuSwap32); R32_UINT -> R16G16_* is a size-compatible vkCmdCopyImage.
inline bool ResidentFormatPair(uint32_t destFormat, uint32_t textureFormat)
{
    const auto raw32 = [](uint32_t f) { return f == 6 || f == 7 || f == 50 || f == 54; };
    if (destFormat == 22 || destFormat == 23)
        return raw32(textureFormat);
    const auto group = [](uint32_t f) { return f == 6 || f == 50 ? 1 : f == 7 || f == 54 ? 2 : f == 10 ? 3 : f == 36 ? 4 : f == 25 ? 5 : 0; };
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
// FM4 Recomp (M7, resident resolves; opt-in FM4_RESOLVE_RESIDENT_COVER=1, texture_vk.cpp ResidentCover): `cover` widens the match
// in two ways, both from the guest layout alone. (1) The resolve's pitch is compared aligned up to 32 texels: its write-back lays the
// words out at that pitch (texture_untile.h TiledOffset2D and front_buffer.h TilesPerRow align it, Xenia texture_util.cc:446), so a
// resolve with RB_COPY_DEST_PITCH 8 stores exactly where one with 32 does. (2) A rectangle that reaches past the texture's (level's)
// texels but stays inside its padded storage (x < the pitch, y < its rows aligned to 32: texture_info.cpp MipStorageOf, Xenia
// texture_util.cc:315-334, a level's stride aligned to 4 KB :342-344) is taken clipped: the texels past it are padding no fetch of
// this texture (level) samples, and their bytes are its own storage, not another level's. FM4's 512x512 k_8_8 chain resolves 8x8
// rectangles into its 4x4, 2x2 and 1x1 levels (0CF00000, 0CF01000, 0CF02000, the hand layout of levels
// 7-9 under the mip address 0CED3000); without `cover` each refusal drops the image's residency and the chain's next draw waits.
// `copy` (when matched): the texels of the rectangle inside the texture (level), from its corner; 0 x 0 when it lies wholly in
// the padding (nothing to copy; the image keeps its texels).
struct ResidentCopy
{
    uint32_t width = 0, height = 0;
};
constexpr uint32_t ResidentAlign32(uint32_t v) { return (v + 31u) & ~31u; }
inline bool ResidentTextureMatches(const ResidentTextureShape& t, const ResidentResolve& r, std::string* why = nullptr, uint32_t* x = nullptr,
                                   uint32_t* y = nullptr, bool cover = false, ResidentCopy* copy = nullptr)
{
    uint32_t cornerX = 0, cornerY = 0;
    const uint32_t resolvePitch = cover ? ResidentAlign32(r.destPitch) : r.destPitch;  // FM4 Recomp (M7, resident resolves): (1) above
    const char* reason = !r.converted                              ? "the resolve has no GPU words"
                         : t.dimension != 1 || t.depth != 1 || t.stacked ? "not a plain 2D texture"
                         : t.levels != 1 || t.packedMips           ? "more than one level"
                         : !t.tiled                                ? "linear"
                         : t.pitch != resolvePitch || t.pitch == 0 || t.pitch % 32 != 0 ? "another pitch"
                         : r.textureBase < t.base || (r.textureBase - t.base) % 4096 != 0 ? "another base"
                         : r.bytesLog2 != 2 && r.textureBase != t.base                    ? "another base"
                                                                                           : nullptr;
    if (reason == nullptr)
    {
        const uint32_t macro = (r.textureBase - t.base) >> 12, tilesPerRow = t.pitch >> 5;
        cornerX = macro % tilesPerRow * 32, cornerY = macro / tilesPerRow * 32;
        // FM4 Recomp (M7, resident resolves): (2) above: past the texels, inside the padded storage, only with `cover`.
        const uint64_t left = uint64_t(cornerX) + r.x0, top = uint64_t(cornerY) + r.y0, right = left + r.width, bottom = top + r.height;
        const bool past = right > t.width || bottom > t.height;
        const bool padded = cover && right <= t.pitch && bottom <= ResidentAlign32(t.height);
        reason = t.endian != r.destEndian                ? "another endian"
                 : !ResidentFormatPair(r.destFormat, t.format) ? "a format whose texel is not the resolve's word"
                 : past && !padded                             ? "the rectangle exceeds the texture"
                                                               : nullptr;
        if (reason == nullptr && copy != nullptr)
        {
            copy->width = right <= t.width ? r.width : left >= t.width ? 0 : uint32_t(t.width - left);
            copy->height = bottom <= t.height ? r.height : top >= t.height ? 0 : uint32_t(t.height - top);
        }
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
// FM4 Recomp (M7, resident resolves): `cover` and `copy` as ResidentTextureMatches, per level.
inline int ResidentLevelFor(const ResidentTextureShape& t, const std::vector<ResidentLevel>& levels, const ResidentResolve& r, std::string* why = nullptr,
                            uint32_t* x = nullptr, uint32_t* y = nullptr, bool cover = false, ResidentCopy* copy = nullptr)
{
    if ((t.levels == 1 && !t.packedMips) || levels.empty())
        return ResidentTextureMatches(t, r, why, x, y, cover, copy) ? 0 : -1;
    std::string first;
    for (size_t level = 0; level < levels.size(); level++)
    {
        const ResidentLevel& l = levels[level];
        std::string reason = "a packed mip level";
        if (!l.packed)
        {
            ResidentTextureShape one = t;
            one.base = l.address, one.width = l.width, one.height = l.height, one.pitch = l.pitch, one.levels = 1, one.packedMips = false;
            if (ResidentTextureMatches(one, r, &reason, x, y, cover, copy))
                return int(level);
        }
        if (level == 0)
            first = reason;
    }
    if (why != nullptr)
        *why = "no level takes it (level 0: " + first + ")";
    return -1;
}

// FM4 Recomp (M7, resident resolves): whether an image that is not resident is in step after the resolve because the resolve replaces
// every texel it samples (level 0 of a single-level texture, the corner at 0,0, the copied extent the whole texture;
// FM4's bloom chain). As before (`cover` false) only when no earlier resolve of the open submission is pending over
// the image. With `cover` also when one is: the write-backs land in record order (edram_vk.cpp Retired: "overlapping destinations end
// as the last resolve wrote them"), so once this resolve's has landed the texture's texels in guest memory are its words whatever the
// earlier ones wrote; until then the image is resident and its draws bind it, never guest memory. FM4's 32x32 k_2_10_10_10 at the bloom
// chain's shared base is resolved after the 64x32 one of the same submission (INFERRED from a race's consumer tally:
// one wait every race frame). Its other in-step paths (a hash or a clean watch) still need no earlier pending resolve.
inline bool ResidentCoversInStep(int level, uint32_t levelCount, uint32_t x, uint32_t y, const ResidentCopy& copy, uint32_t width, uint32_t height,
                                 bool noEarlierPending, bool cover)
{
    const bool covers = level == 0 && levelCount == 1 && x == 0 && y == 0 && copy.width == width && copy.height == height;
    return covers && (noEarlierPending || cover);
}

}  // namespace runtime::video
