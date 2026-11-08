// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// texture_info.h (see its header comment for sources and attribution).
#include "texture_info.h"
#include "texture_untile.h"
#include <algorithm>
#include <bit>

namespace runtime::video {
namespace {

// Xenia texture_info_formats.inl at 67f16c4e315f (name, block width, block height, bits per texel).
constexpr FormatInfo kFormats[64] = {
    {"k_1_REVERSE", 1, 1, 1}, {"k_1", 1, 1, 1}, {"k_8", 1, 1, 8}, {"k_1_5_5_5", 1, 1, 16}, {"k_5_6_5", 1, 1, 16},
    {"k_6_5_5", 1, 1, 16}, {"k_8_8_8_8", 1, 1, 32}, {"k_2_10_10_10", 1, 1, 32}, {"k_8_A", 1, 1, 8}, {"k_8_B", 1, 1, 8},
    {"k_8_8", 1, 1, 16}, {"k_Cr_Y1_Cb_Y0_REP", 2, 1, 16}, {"k_Y1_Cr_Y0_Cb_REP", 2, 1, 16}, {"k_16_16_EDRAM", 1, 1, 32},
    {"k_8_8_8_8_A", 1, 1, 32}, {"k_4_4_4_4", 1, 1, 16}, {"k_10_11_11", 1, 1, 32}, {"k_11_11_10", 1, 1, 32}, {"k_DXT1", 4, 4, 4},
    {"k_DXT2_3", 4, 4, 8}, {"k_DXT4_5", 4, 4, 8}, {"k_16_16_16_16_EDRAM", 1, 1, 64}, {"k_24_8", 1, 1, 32}, {"k_24_8_FLOAT", 1, 1, 32},
    {"k_16", 1, 1, 16}, {"k_16_16", 1, 1, 32}, {"k_16_16_16_16", 1, 1, 64}, {"k_16_EXPAND", 1, 1, 16}, {"k_16_16_EXPAND", 1, 1, 32},
    {"k_16_16_16_16_EXPAND", 1, 1, 64}, {"k_16_FLOAT", 1, 1, 16}, {"k_16_16_FLOAT", 1, 1, 32}, {"k_16_16_16_16_FLOAT", 1, 1, 64},
    {"k_32", 1, 1, 32}, {"k_32_32", 1, 1, 64}, {"k_32_32_32_32", 1, 1, 128}, {"k_32_FLOAT", 1, 1, 32}, {"k_32_32_FLOAT", 1, 1, 64},
    {"k_32_32_32_32_FLOAT", 1, 1, 128}, {"k_32_AS_8", 4, 1, 8}, {"k_32_AS_8_8", 2, 1, 16}, {"k_16_MPEG", 1, 1, 16},
    {"k_16_16_MPEG", 1, 1, 32}, {"k_8_INTERLACED", 1, 1, 8}, {"k_32_AS_8_INTERLACED", 4, 1, 8}, {"k_32_AS_8_8_INTERLACED", 1, 1, 16},
    {"k_16_INTERLACED", 1, 1, 16}, {"k_16_MPEG_INTERLACED", 1, 1, 16}, {"k_16_16_MPEG_INTERLACED", 1, 1, 32}, {"k_DXN", 4, 4, 8},
    {"k_8_8_8_8_AS_16_16_16_16", 1, 1, 32}, {"k_DXT1_AS_16_16_16_16", 4, 4, 4}, {"k_DXT2_3_AS_16_16_16_16", 4, 4, 8},
    {"k_DXT4_5_AS_16_16_16_16", 4, 4, 8}, {"k_2_10_10_10_AS_16_16_16_16", 1, 1, 32}, {"k_10_11_11_AS_16_16_16_16", 1, 1, 32},
    {"k_11_11_10_AS_16_16_16_16", 1, 1, 32}, {"k_32_32_32_FLOAT", 1, 1, 96}, {"k_DXT3A", 4, 4, 4}, {"k_DXT5A", 4, 4, 4},
    {"k_CTX1", 4, 4, 4}, {"k_DXT3A_AS_1_1_1_1", 4, 4, 4}, {"k_8_8_8_8_GAMMA_EDRAM", 1, 1, 32}, {"k_2_10_10_10_FLOAT_EDRAM", 1, 1, 32},
};

constexpr uint32_t kPhysicalSize = 0x20000000;  // 512 MB (kernel/virtual_memory.h kPhysicalSize)

template <typename Result>
bool Refuse(Result& r, std::string why)
{
    r.ok = false;
    r.error = std::move(why);
    return false;
}

// xe::log2_floor / log2_ceil / next_pow2 for values >= 1.
constexpr uint32_t Log2Floor(uint32_t v) { return uint32_t(std::bit_width(v)) - 1; }
constexpr uint32_t Log2Ceil(uint32_t v) { return uint32_t(std::bit_width(v - 1)); }
constexpr uint32_t NextPow2(uint32_t v) { return uint32_t(std::bit_ceil(v)); }

// Levels 1.. are stored like mips of a texture whose base size is the next power of two
// (texture_util.cc:310-313); rows and pitches aligned to 32 blocks (:315-334); linear mip rows to
// 256 bytes (:324-327); a level's stride aligned to 4 KB (:342-344).
struct MipStorage
{
    uint32_t pitchBytes, rows, pitchBlocks;
    size_t strideBytes;
};
MipStorage MipStorageOf(const TextureFetch& f, const FormatInfo& fi, uint32_t level)
{
    const uint32_t pitchTexels = std::max(NextPow2(f.width) >> level, 1u), rowTexels = std::max(NextPow2(f.height) >> level, 1u);
    MipStorage s{};
    s.pitchBlocks = AlignUp(AlignUp(pitchTexels, fi.blockWidth) / fi.blockWidth, kTileWidthHeight);
    s.pitchBytes = s.pitchBlocks * BytesPerBlock(fi);
    if (!f.tiled)
        s.pitchBytes = AlignUp(s.pitchBytes, 256), s.pitchBlocks = s.pitchBytes / BytesPerBlock(fi);
    s.rows = AlignUp(AlignUp(rowTexels, fi.blockHeight) / fi.blockHeight, kTileWidthHeight);
    s.strideBytes = (size_t(s.pitchBytes) * s.rows + 4095) & ~size_t(4095);
    return s;
}

bool LevelLayout(TextureDescription& d, uint32_t level, MipLevel& out)
{
    const TextureFetch& f = d.fetch;
    const FormatInfo& fi = d.info;
    const uint32_t log2 = d.log2BytesPerBlock;
    out = {};
    out.packed = level >= d.packedLevel;
    out.widthTexels = std::max(d.width >> level, 1u), out.heightTexels = std::max(d.height >> level, 1u);
    out.widthBlocks = AlignUp(out.widthTexels, fi.blockWidth) / fi.blockWidth;
    out.heightBlocks = AlignUp(out.heightTexels, fi.blockHeight) / fi.blockHeight;
    if (level == 0)
    {
        // texture_util.cc:306-308: the fetch constant's pitch, no re-alignment of linear rows.
        out.address = f.baseAddress;
        out.pitchBlocks = TiledPitchBlocks(f.pitchTexels, fi.blockWidth);
        out.rowPitchBytes = out.pitchBlocks << log2;
        // 3D (DescribeTexture admits level 0 only): texture_util.cc:306-308 (the base's z slice stride is its height in
        // texel rows), :328-333 (in blocks, aligned to 32), :403 (z extent = the depth).
        out.depth = d.depth;
        out.zStrideBlockRows = AlignUp(AlignUp(d.height, fi.blockHeight) / fi.blockHeight, kTileWidthHeight);
    }
    else
    {
        // Level k (k <= packed level) follows the strides of levels 1..k-1 under the mip address;
        // a level past the packed level lives in the tail stored as the packed level (:276-293).
        const uint32_t stored = std::min(level, d.packedLevel);
        uint64_t offset = 0;
        for (uint32_t k = 1; k < stored; k++)
            offset += MipStorageOf(f, fi, k).strideBytes;
        const MipStorage s = MipStorageOf(f, fi, stored);
        if (uint64_t(f.mipAddress) + offset > 0xFFFFFFFFull)
            return Refuse(d, "mip level " + std::to_string(level) + " address overflows");
        out.address = uint32_t(f.mipAddress + offset);
        out.pitchBlocks = s.pitchBlocks;
        out.rowPitchBytes = s.pitchBytes;
    }
    if (out.packed)
    {
        uint32_t xTexels = 0, yTexels = 0;
        PackedMipOffsetTexels(f.width, f.height, level, xTexels, yTexels);
        if (xTexels % fi.blockWidth || yTexels % fi.blockHeight)
            return Refuse(d, "packed mip offset of level " + std::to_string(level) + " inside a block");
        out.x = xTexels / fi.blockWidth, out.y = yTexels / fi.blockHeight;
    }
    if (out.x + out.widthBlocks > out.pitchBlocks)
        return Refuse(d, "level " + std::to_string(level) + " is wider than its pitch (" + std::to_string(out.x + out.widthBlocks) + " > " +
                             std::to_string(out.pitchBlocks) + " blocks)");
    if (d.dimension == 2)  // texture_util.cc:406-412 (tiled: GetTiledAddressUpperBound3D with height = the y extent), :420-423
        out.extentBytes = f.tiled ? TiledAddressUpperBound3D(out.widthBlocks, out.heightBlocks, out.depth, out.pitchBlocks, out.heightBlocks, log2)
                                  : size_t(out.rowPitchBytes) * out.zStrideBlockRows * (out.depth - 1) +
                                        LinearExtent(out.widthBlocks, out.heightBlocks, out.rowPitchBytes, log2);
    else if (f.tiled)
        out.extentBytes = TiledAddressUpperBound2D(out.x + out.widthBlocks, out.y + out.heightBlocks, out.pitchBlocks, log2);
    else
        out.extentBytes = size_t(out.rowPitchBytes) * out.y + LinearExtent(out.x + out.widthBlocks, out.heightBlocks, out.rowPitchBytes, log2);
    if (uint64_t(out.address) + out.extentBytes > kPhysicalSize)
        return Refuse(d, "level " + std::to_string(level) + " ends past the physical memory");
    return true;
}

}  // namespace

const FormatInfo& GetFormatInfo(uint32_t format) { return kFormats[format & 63]; }

bool HostFormatFor(uint32_t format, HostFormat& out)
{
    // vulkan_texture_cache.cc kBestHostFormats: k_8 :100-104, k_8_8_8_8 :121-125, k_DXT1 :186-190,
    // k_8_8_8_8_AS_16_16_16_16 :335-339, k_DXT1_AS_16_16_16_16 :340-344,
    // k_2_10_10_10_AS_16_16_16_16 :355-360 (the unsigned entries; signed data is refused).
    switch (format)
    {
    case 2: out = {vk::kFormatR8Unorm, kSwizzleRRRR, false}; return true;                     // k_8
    case 6: case 50: out = {vk::kFormatR8G8B8A8Unorm, kSwizzleRGBA, false}; return true;      // k_8_8_8_8 (+ AS_16_16_16_16)
    case 18: case 51: out = {vk::kFormatBc1RgbaUnormBlock, kSwizzleRGBA, true}; return true;  // k_DXT1 (+ AS_16_16_16_16)
    case 54: out = {vk::kFormatA2B10G10R10UnormPack32, kSwizzleRGBA, false}; return true;    // k_2_10_10_10_AS_16_16_16_16
    case 20: case 53: out = {vk::kFormatBc3UnormBlock, kSwizzleRGBA, true}; return true;      // k_DXT4_5 :196-200, k_DXT4_5_AS_16_16_16_16 :350-354 (BC3, 128bpb load)
    case 36: out = {vk::kFormatR32Sfloat, kSwizzleRRRR, false, true, true}; return true;      // k_32_FLOAT :275-279 (R32_SFLOAT, RRRR)
    case 23: out = {vk::kFormatR32Sfloat, kSwizzleRRRR, false, false, true, true}; return true; // k_24_8_FLOAT :210-213 (depth float load)
    default: out = {}; return false;
    }
}

uint32_t PackedMipLevel(uint32_t width, uint32_t height, bool packedMips)
{
    if (!packedMips)
        return UINT32_MAX;
    const uint32_t log2Size = Log2Ceil(std::min(width, height));  // texture_util.h:36-39
    return log2Size > 4 ? log2Size - 4 : 0;
}

void PackedMipOffsetTexels(uint32_t width, uint32_t height, uint32_t level, uint32_t& x, uint32_t& y)
{
    // texture_util.cc:144-181 (the Z branch, :182-192, is not reached for 2D): levels packed..+2
    // along one axis at 16, 8, 4; later ones along the other at (1 << (log2 of the larger side -
    // packed base)) >> (packed - 2). Square textures take the "taller than wide" branch (:159).
    const uint32_t log2W = Log2Ceil(width), log2H = Log2Ceil(height);
    const uint32_t log2Size = std::min(log2W, log2H);
    const uint32_t base = log2Size > 4 ? log2Size - 4 : 0;
    const uint32_t packed = level - base;
    const bool wide = log2W > log2H;
    x = y = 0;
    if (packed < 3)
        (wide ? y : x) = 16u >> packed;
    else
        (wide ? x : y) = (1u << ((wide ? log2W : log2H) - base)) >> (packed - 2);
}

TextureDescription DescribeTexture(const uint32_t fetch[6])
{
    TextureDescription d;
    d.fetch = DecodeTextureFetch(fetch);
    const TextureFetch& f = d.fetch;
    d.info = GetFormatInfo(f.format);
    const std::string name = d.info.name;

    if (f.type != 2)
        Refuse(d, "fetch constant type " + std::to_string(f.type) + " is not a texture (2)");
    else if (f.dimension != 1 && f.dimension != 2)
        Refuse(d, "texture dimension " + std::to_string(f.dimension) + " (0 1D, 3 cube) is not supported; only 2D (1) and 3D (2)");
    else if (f.stacked)
        Refuse(d, "a stacked 2D texture (" + std::to_string(f.stackDepth + 1) + " layers) is not supported");
    else if (!HostFormatFor(f.format, d.host))
        Refuse(d, "texture format " + std::to_string(f.format) + " (" + name + ") has no host format yet");
    else if (f.baseAddress == 0)
        Refuse(d, "base address 0 (Xenia would skip the base level)");
    else if (f.baseAddress >= kPhysicalSize)
        Refuse(d, "base address past the 512 MB physical memory");
    if (!d.error.empty())
        return d;

    const uint32_t bytesPerBlock = BytesPerBlock(d.info);
    d.log2BytesPerBlock = Log2Floor(bytesPerBlock);
    if (!EndianSupported(d.log2BytesPerBlock, f.endian))
    {
        Refuse(d, "endian " + std::to_string(f.endian) + " on " + std::to_string(bytesPerBlock) + "-byte blocks (" + name +
                      ") has no load rule (texture_untile.h)");
        return d;
    }

    // The size: dword 2 as size_2d or, for 3D, size_3d (11/11/10 bits; xenos.h:1255-1271; texture_util.cc:15-64
    // GetSubresourcesFromFetchConstant :36-41). A stacked 2D (an array) is refused above; 3D with stacked set is an
    // assert in Xenia (:37) and refused the same way.
    const bool volume = f.dimension == 2;
    d.dimension = f.dimension;
    d.width = volume ? f.width3D : f.width, d.height = volume ? f.height3D : f.height, d.depth = volume ? f.depth3D : 1;
    // Levels (texture_util.cc:60-92; the longest axis includes the depth for 3D, :61-65).
    const uint32_t sizeMax = Log2Floor(std::max(std::max(d.width, d.height), d.depth));
    uint32_t first = 0, last = 0;
    if (f.mipAddress != 0)
    {
        first = std::min(f.mipMinLevel, sizeMax);
        last = std::max(std::min(f.mipMaxLevel, sizeMax), first);
    }
    if (last != 0 && first != 0)
    {
        Refuse(d, "first mip level " + std::to_string(first) + " above 0 (Xenia would ignore the base level)");
        return d;
    }
    if (last != 0 && f.mipAddress >= kPhysicalSize)
    {
        Refuse(d, "mip address past the 512 MB physical memory");
        return d;
    }
    if (volume && last != 0)
    {
        Refuse(d, "a 3D texture with mip levels 0-" + std::to_string(last) + " (only its base level is modelled)");
        return d;
    }
    d.levelCount = last + 1;
    d.packedLevel = PackedMipLevel(d.width, d.height, f.packedMips);
    if (volume && d.packedLevel == 0)
    {
        Refuse(d, "a 3D texture whose base is in a packed mip tail (" + std::to_string(d.width) + "x" + std::to_string(d.height) +
                      " with packed mips; texture_util.cc:108-198 packed offsets are not modelled for 3D)");
        return d;
    }
    d.levels.resize(d.levelCount);
    for (uint32_t level = 0; level < d.levelCount; level++)
        if (!LevelLayout(d, level, d.levels[level]))
            return d;
    d.base = {f.baseAddress, uint32_t(d.levels[0].extentBytes)};
    if (d.levelCount > 1)
    {
        uint64_t end = 0;
        for (uint32_t level = 1; level < d.levelCount; level++)
            end = std::max(end, uint64_t(d.levels[level].address) - f.mipAddress + d.levels[level].extentBytes);
        d.mips = {f.mipAddress, uint32_t(end)};
    }
    d.ok = true;
    return d;
}

TextureBinding DescribeBinding(const uint32_t fetch[6], const HostFormat& host, const ShaderFetch& shader)
{
    TextureBinding b;
    const TextureFetch f = DecodeTextureFetch(fetch);
    // ucode fetch dimension 1 (2D) needs a 2D constant (dimension 1), 2 (3D or stacked) a 3D constant (dimension 2): a stacked
    // 2D array sampled as 3D is refused by the storage description (stacked), a cube (3) by both.
    if (!((shader.dimension == 1 && f.dimension == 1) || (shader.dimension == 2 && f.dimension == 2)))
    {
        Refuse(b, "the shader samples with dimension " + std::to_string(shader.dimension) + " but the constant's dimension is " +
                      std::to_string(f.dimension) + "; only 2D fetches of 2D constants and 3D fetches of 3D constants are supported");
        return b;
    }
    // The number-format bit (dword 3 bit 0): Xenia 67f16c4 declares it (xenos.h:1274) but neither its texture cache nor its
    // shader translator reads it; for float data it has no meaning (FM4's k_32_FLOAT luminance texture sets it), so it is
    // ignored for float formats only (HostFormat::floatData) and still refused for normalized ones.
    if (f.numFormat != 0 && !host.floatData)
    {
        Refuse(b, "integer number format (dword 3 bit 0) is not supported");
        return b;
    }

    // Components and signs (texture_util.cc:554-573 SwizzleSigns: the sign is the source component's).
    bool gammaSeen = false, nonConstantXyz = false;
    for (uint32_t i = 0; i < 4; i++)
    {
        const uint32_t g = SwizzleComponent(f.swizzle, i);
        if (g > 5)
        {
            Refuse(b, "swizzle component " + std::to_string(i) + " has the undefined value " + std::to_string(g));
            return b;
        }
        if (g >= 4)
        {
            b.components[i] = g == 4 ? vk::kSwizzleZero : vk::kSwizzleOne;
            continue;
        }
        b.components[i] = vk::kSwizzleR + SwizzleComponent(host.formatSwizzle, g);
        const uint32_t sign = f.sign[g];
        if ((sign == 1 || sign == 2) && !host.signIgnored)
        {
            Refuse(b, std::string(sign == 1 ? "signed" : "unsigned biased") + " component " + std::to_string(g) + " is not supported");
            return b;
        }
        if (i == 3)
        {
            if (sign == 3)
            {
                Refuse(b, "gamma on the w component is not supported (XE_TEXTURE_<slot>_GAMMA converts x, y, z)");
                return b;
            }
            continue;
        }
        if (nonConstantXyz && gammaSeen != (sign == 3))
        {
            Refuse(b, "mixed gamma and linear components in x, y, z");
            return b;
        }
        nonConstantXyz = true, gammaSeen = sign == 3;
    }
    b.gamma = nonConstantXyz && gammaSeen;
    b.expAdjust = f.expAdjust;
    b.lodBias = float(f.lodBias) / 32.0f;

    // Sampler (vulkan_texture_cache.cc:611-690, :755-820): the z clamp is the constant's for 3D, clamp-to-edge for 2D
    // (texture_util.cc:597-618 GetClampModesForDimension); modes Xenia only approximates (:2648-2665) are refused.
    uint32_t* modes[3] = {&b.sampler.addressModeU, &b.sampler.addressModeV, &b.sampler.addressModeW};
    b.sampler.addressModeW = vk::kAddressClampToEdge;
    bool border = false;
    for (uint32_t axis = 0; axis < (f.dimension == 2 ? 3u : 2u); axis++)
    {
        switch (f.clamp[axis])
        {
        case 0: *modes[axis] = vk::kAddressRepeat; break;
        case 1: *modes[axis] = vk::kAddressMirroredRepeat; break;
        case 2: *modes[axis] = vk::kAddressClampToEdge; break;
        case 6: *modes[axis] = vk::kAddressClampToBorder, border = true; break;
        default:
            Refuse(b, "clamp mode " + std::to_string(f.clamp[axis]) + " on axis " + std::to_string(axis) + " (Xenia approximates it; refused here)");
            return b;
        }
    }
    if (border)
    {
        if (f.borderColor > 1)
        {
            Refuse(b, "border colour " + std::to_string(f.borderColor) + " (YCbCr) is not supported");
            return b;
        }
        b.sampler.borderColor = f.borderColor == 1 ? vk::kBorderFloatOpaqueWhite : vk::kBorderFloatTransparentBlack;
    }
    const uint32_t mag = shader.mag == 3 ? f.magFilter : shader.mag, min = shader.min == 3 ? f.minFilter : shader.min,
                   mip = shader.mip == 3 ? f.mipFilter : shader.mip, aniso = shader.aniso == 7 ? f.anisoFilter : shader.aniso;
    if (mag > 1 || min > 1)
    {
        Refuse(b, "mag filter " + std::to_string(mag) + " / min filter " + std::to_string(min) + " (only point 0 and linear 1)");
        return b;
    }
    if (mip > 2)
    {
        Refuse(b, "mip filter " + std::to_string(mip) + " is unresolved (3 = fetch constant, which says 3)");
        return b;
    }
    if (aniso == 6 || aniso == 7)
    {
        Refuse(b, "anisotropic filter " + std::to_string(aniso) + " is undefined or unresolved");
        return b;
    }
    // The level range as DescribeTexture (texture_util.cc:60-92): minLod is the first level, 0 here
    // (a first level above 0 is refused by the storage description; the sampler mirrors that).
    b.sampler.magFilter = mag == 1 ? vk::kFilterLinear : vk::kFilterNearest;
    b.sampler.minFilter = min == 1 ? vk::kFilterLinear : vk::kFilterNearest;
    b.sampler.mipmapMode = mip == 1 ? vk::kMipmapModeLinear : vk::kMipmapModeNearest;
    b.sampler.anisotropyEnable = aniso != 0;
    b.sampler.maxAnisotropy = aniso != 0 ? float(1u << (aniso - 1)) : 1.0f;
    b.sampler.minLod = 0.0f;
    b.sampler.maxLod = mip == 2 ? 0.25f : vk::kLodClampNone;
    b.ok = true;
    return b;
}

}  // namespace runtime::video
