// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ForzaRecomp contributors
// Copyright (C) 2026 FM4 Recomp contributors
//
// A texture fetch constant (six dwords of SHADER_CONSTANT_FETCH_xx, host order) decoded into two
// validated descriptions, or a refusal naming what is not modelled (fail closed; nothing is
// approximated silently). M4 unit 7, CPU side.
//   - TextureDescription (DescribeTexture): the *storage*: format, host image format, size, levels,
//     the guest byte ranges the texture occupies. It depends only on the fields TextureKey
//     (texture_cache.h) holds, so the cache computes it once per key.
//   - TextureBinding (DescribeBinding): the *per-draw* part: component mapping, gamma, exponent
//     bias, LOD bias, the sampler, and the refusals that depend on the shader's fetch instruction
//     (its dimension and filter overrides) or on fields outside the key (signs, swizzle, clamps,
//     filters). Computed at every draw: bit arithmetic, no level loop.
//
// Field layout: Xenia xenia-canary 67f16c4e315f src/xenia/gpu/xenos.h:1206-1307
// (xe_gpu_texture_fetch_t). Level range: texture_util.cc:15-106 (GetSubresourcesFromFetchConstant),
// where Xenia *adjusts* (base page 0, first level above 0) this refuses. Level storage:
// texture_util.cc:200-440 (GetGuestTextureLayout) and :108-198 (GetPackedMipOffset). Host formats and
// their component swizzle: vulkan/vulkan_texture_cache.cc:91-395 (kBestHostFormats). Sampler:
// vulkan/vulkan_texture_cache.cc:611-690 (GetSamplerParameters), :755-820 (the VkSamplerCreateInfo),
// :2648-2665 (NormalizeClampMode; the modes it approximates are refused here).
//
// DecodeTextureFetch, the mip-level layout (PackedMipLevel, PackedMipOffsetTexels, MipLevel, the
// level storage rules) and HostSwizzle are taken from ForzaRecomp runtime/gpu/texture.h:259-520
// (GPL-3.0-or-later, same copyright holder), re-derived here against the Xenia lines above (the tests'
// expected values are hand-computed from those lines, never from this code). FM4 Recomp changes:
// namespace runtime::video; storage and binding as two entry points returning a reason; the format
// table is data (all 64 Xenos formats from texture_info_formats.inl, host mapping only for the
// formats FM4's first screens use); the sampler is described with Vulkan's numeric values (no Vulkan
// header: the CPU tests build without one; renderer.cpp static_asserts the mirrors); level 0 and the
// mips are reported as the two guest ranges the write watch protects (texture_cache.h).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime::video {

// ---- Vulkan numeric values this header mirrors (vulkan_core.h; the renderer static_asserts them) --
namespace vk {
constexpr uint32_t kFormatUndefined = 0, kFormatR8Unorm = 9, kFormatR8G8B8A8Unorm = 37, kFormatA2B10G10R10UnormPack32 = 64,
                   kFormatR32Sfloat = 100, kFormatBc1RgbaUnormBlock = 133,
                   kFormatBc3UnormBlock = 137;
constexpr uint32_t kFilterNearest = 0, kFilterLinear = 1;
constexpr uint32_t kMipmapModeNearest = 0, kMipmapModeLinear = 1;
constexpr uint32_t kAddressRepeat = 0, kAddressMirroredRepeat = 1, kAddressClampToEdge = 2, kAddressClampToBorder = 3;
constexpr uint32_t kBorderFloatTransparentBlack = 0, kBorderFloatOpaqueWhite = 4;
constexpr uint32_t kSwizzleZero = 1, kSwizzleOne = 2, kSwizzleR = 3, kSwizzleG = 4, kSwizzleB = 5, kSwizzleA = 6;
constexpr float kLodClampNone = 1000.0f;
}  // namespace vk

// ---- The fetch constant's fields (xenos.h:1206-1307) ---------------------------------------------
// Enumerations: type 2 = texture (FetchConstantType, :1124-1129); sign 0 unsigned, 1 signed, 2
// unsigned biased, 3 gamma (:121-129); clamp 0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 mirror
// clamp to edge, 4 clamp to halfway, 5 mirror clamp to halfway, 6 clamp to border, 7 mirror clamp to
// border (:104-113); endian 0 none, 1 8in16, 2 8in32, 3 16in32 (:193-198); dimension 0 1D, 1 2D or
// stacked, 2 3D, 3 cube (:97-102); swizzle 3 bits per component, 0-3 = X Y Z W, 4 zero, 5 one;
// filters 0 point, 1 linear, 2 base map (mip only), 3 from the fetch instruction (:131-138); aniso 0
// disabled, 1-5 max 1:1 to 16:1, 7 from the instruction (:140-148); border colour 0 ABGR black,
// 1 ABGR white, 2-3 YCbCr blacks (:150-165).
struct TextureFetch
{
    uint32_t type;               // dword 0 bits 0-1
    uint32_t sign[4];            // dword 0 bits 2-9, 2 bits each (x y z w)
    uint32_t clamp[3];           // dword 0 bits 10-18, 3 bits each (x y z)
    uint32_t signedRfMode;       // dword 0 bit 19
    uint32_t pitchTexels;        // dword 0 bits 22-30, << 5 (texels, not blocks)
    bool tiled;                  // dword 0 bit 31
    uint32_t format;             // dword 1 bits 0-5 (xenos::TextureFormat)
    uint32_t endian;             // dword 1 bits 6-7
    uint32_t requestSize;        // dword 1 bits 8-9
    bool stacked;                // dword 1 bit 10
    uint32_t nearestClampPolicy; // dword 1 bit 11
    uint32_t baseAddress;        // dword 1 bits 12-31, << 12 (physical)
    uint32_t width, height;      // dword 2 (2D / cube): bits 0-12 and 13-25, each + 1
    uint32_t stackDepth;         // dword 2 bits 26-31 (2D)
    uint32_t width3D, height3D, depth3D;  // dword 2 read as size_3d: bits 0-10, 11-21, 22-31, each + 1
    uint32_t numFormat;          // dword 3 bit 0 (0 fraction, 1 integer)
    uint32_t swizzle;            // dword 3 bits 1-12
    int32_t expAdjust;           // dword 3 bits 13-18, signed
    uint32_t magFilter, minFilter, mipFilter;  // dword 3 bits 19-20, 21-22, 23-24
    uint32_t anisoFilter;        // dword 3 bits 25-27
    uint32_t arbitraryFilter;    // dword 3 bits 28-30
    uint32_t borderSize;         // dword 3 bit 31
    uint32_t volMagFilter, volMinFilter;  // dword 4 bits 0, 1
    uint32_t mipMinLevel, mipMaxLevel;    // dword 4 bits 2-5, 6-9
    int32_t lodBias;             // dword 4 bits 12-21, signed, 5 fraction bits
    uint32_t borderColor;        // dword 5 bits 0-1
    uint32_t dimension;          // dword 5 bits 9-10
    bool packedMips;             // dword 5 bit 11
    uint32_t mipAddress;         // dword 5 bits 12-31, << 12 (physical)
};

constexpr TextureFetch DecodeTextureFetch(const uint32_t d[6])
{
    TextureFetch f{};
    f.type = d[0] & 3;
    for (uint32_t i = 0; i < 4; i++)
        f.sign[i] = (d[0] >> (2 + 2 * i)) & 3;
    for (uint32_t i = 0; i < 3; i++)
        f.clamp[i] = (d[0] >> (10 + 3 * i)) & 7;
    f.signedRfMode = (d[0] >> 19) & 1;
    f.pitchTexels = ((d[0] >> 22) & 0x1FF) << 5;
    f.tiled = (d[0] >> 31) != 0;
    f.format = d[1] & 0x3F;
    f.endian = (d[1] >> 6) & 3;
    f.requestSize = (d[1] >> 8) & 3;
    f.stacked = ((d[1] >> 10) & 1) != 0;
    f.nearestClampPolicy = (d[1] >> 11) & 1;
    f.baseAddress = d[1] & 0xFFFFF000u;
    f.width = (d[2] & 0x1FFF) + 1;
    f.height = ((d[2] >> 13) & 0x1FFF) + 1;
    f.stackDepth = d[2] >> 26;
    f.width3D = (d[2] & 0x7FF) + 1;
    f.height3D = ((d[2] >> 11) & 0x7FF) + 1;
    f.depth3D = (d[2] >> 22) + 1;
    f.numFormat = d[3] & 1;
    f.swizzle = (d[3] >> 1) & 0xFFF;
    f.expAdjust = int32_t(((d[3] >> 13) & 0x3F) ^ 0x20) - 0x20;  // sign-extend 6 bits
    f.magFilter = (d[3] >> 19) & 3;
    f.minFilter = (d[3] >> 21) & 3;
    f.mipFilter = (d[3] >> 23) & 3;
    f.anisoFilter = (d[3] >> 25) & 7;
    f.arbitraryFilter = (d[3] >> 28) & 7;
    f.borderSize = d[3] >> 31;
    f.volMagFilter = d[4] & 1;
    f.volMinFilter = (d[4] >> 1) & 1;
    f.mipMinLevel = (d[4] >> 2) & 0xF;
    f.mipMaxLevel = (d[4] >> 6) & 0xF;
    f.lodBias = int32_t(((d[4] >> 12) & 0x3FF) ^ 0x200) - 0x200;  // sign-extend 10 bits
    f.borderColor = d[5] & 3;
    f.dimension = (d[5] >> 9) & 3;
    f.packedMips = ((d[5] >> 11) & 1) != 0;
    f.mipAddress = d[5] & 0xFFFFF000u;
    return f;
}

// ---- The Xenos texture formats (Xenia texture_info_formats.inl, all 64; texture_info.h:191-222) ---
struct FormatInfo
{
    const char* name;
    uint8_t blockWidth, blockHeight;  // texels per block
    uint8_t bitsPerTexel;             // bytes per block = blockWidth * blockHeight * bitsPerTexel / 8
};
const FormatInfo& GetFormatInfo(uint32_t format);  // format 0-63
constexpr uint32_t BytesPerBlock(const FormatInfo& f) { return uint32_t(f.blockWidth) * f.blockHeight * f.bitsPerTexel / 8; }

// The host image a guest format is loaded into, and the host format's own component swizzle
// (same encoding as the fetch constant's: what each guest component reads from the host image;
// kBestHostFormats' third member). Only the formats FM4's first screens use are mapped
// every other format is refused by DescribeTexture.
struct HostFormat
{
    uint32_t vkFormat = vk::kFormatUndefined;
    uint32_t formatSwizzle = 0;   // e.g. RRRR for k_8 (R8_UNORM), RGBA for k_8_8_8_8
    bool blockCompressed = false; // BC1 / BC3: 4x4 texel blocks of 8 / 16 bytes; needs textureCompressionBC
    // Float data: the fetch's sign field changes nothing (Xenia maps both signednesses to one format, unsigned_signed_compatible:
    // vulkan_texture_cache.cc:275-279 k_32_FLOAT), so signed components are not refused; nor is the number-format bit.
    bool signIgnored = false;
    // Float data: the number-format bit (dword 3 bit 0) is ignored (texture_info.cpp DescribeBinding). k_32_FLOAT and
    // k_24_8_FLOAT; the sign rule above stays separate (k_24_8_FLOAT has no signed host format in Xenia).
    bool floatData = false;
    // FM4 Recomp: k_24_8_FLOAT: each 32-bit word w (host order) is uploaded as the float32 of its 20e4 depth, Float20e4To32(w >> 8)
    // (Xenia vulkan_texture_cache.cc:210-213 kLoadShaderIndexDepthFloat -> R32_SFLOAT, RRRR; texture_load_depth_float.cs.xesl
    // XE_TEXTURE_LOAD_32BPB_TRANSFORM = XeFloat20e4To32(blocks >> 8), pixel_formats.xesli:622-638, no [0, 0.5) remap),
    // = edram_vk_plan.h ColorWordDepth(w, D24FS8). The stencil (bits 0-7) is not sampled.
    bool depth20e4 = false;
};
bool HostFormatFor(uint32_t format, HostFormat& out);

constexpr uint32_t kSwizzleRRRR = 0;                                 // 0 | 0 << 3 | 0 << 6 | 0 << 9
constexpr uint32_t kSwizzleRGBA = 0 | 1 << 3 | 2 << 6 | 3 << 9;     // 0x688
constexpr uint32_t SwizzleComponent(uint32_t swizzle, uint32_t i) { return (swizzle >> (3 * i)) & 7; }

// ---- One stored mip level (ForzaRecomp texture.h MipLevel) ---------------------------------------
struct MipLevel
{
    uint32_t address = 0;         // physical address of the level's storage (its tail when packed)
    uint32_t pitchBlocks = 0;     // tiled: the row pitch in blocks (a multiple of 32)
    uint32_t rowPitchBytes = 0;   // linear: the row pitch in bytes
    uint32_t x = 0, y = 0;        // the level's first block inside its storage (0, 0 unless packed)
    uint32_t widthBlocks = 0, heightBlocks = 0;  // the level's own size in blocks
    uint32_t widthTexels = 0, heightTexels = 0;  // the level's own size in texels (the host image level)
    uint32_t depth = 1;                // 3D: slices (the level's z extent); 1 otherwise
    uint32_t zStrideBlockRows = 0;     // 3D: the slice stride in block rows, aligned to 32 (texture_util.cc:328-333)
    size_t extentBytes = 0;       // bytes from `address` the copy reads
    bool packed = false;
};

// The packed mip level (UINT32_MAX without packed mips): texture_util.h:36-39 GetPackedMipLevel.
uint32_t PackedMipLevel(uint32_t width, uint32_t height, bool packedMips);
// The texel offset of `level` (>= the packed level) in its tail: texture_util.cc:144-193 (2D).
void PackedMipOffsetTexels(uint32_t width, uint32_t height, uint32_t level, uint32_t& x, uint32_t& y);

// ---- The storage description -----------------------------------------------------------------------
struct GuestRange
{
    uint32_t physical = 0, size = 0;  // size 0: none
};

struct TextureDescription
{
    bool ok = false;
    std::string error;  // why not (every refusal names its field and value)
    TextureFetch fetch{};
    FormatInfo info{};
    HostFormat host{};
    uint32_t log2BytesPerBlock = 0;
    uint32_t width = 0, height = 0;       // level 0, texels
    uint32_t depth = 1;                   // 3D: slices; 1 for 2D
    uint32_t dimension = 1;               // the fetch constant's (1 2D, 2 3D; others are refused)
    uint32_t levelCount = 0;              // host image levels, from level 0 (never a first level above 0)
    uint32_t packedLevel = UINT32_MAX;
    std::vector<MipLevel> levels;         // levelCount entries, level 0 first
    GuestRange base, mips;                // the write watch's ranges: level 0, and levels 1.. (size 0: none)
};

// Decodes and validates the storage of `fetch` (the six host-order dwords). Refuses (ok false,
// error) anything outside the modelled subset: type not texture, a dimension other than 2D or 3D
// (1D, cube and stacked 2D arrays: not yet), a 3D texture with mips or a packed base, a format without a host mapping, an endian the block size has no
// rule for, a base address of 0 or outside the 512 MB physical memory, a first level above 0, a
// level wider than its pitch, a level that overflows the physical memory, a packed-mip offset
// inside a block.
TextureDescription DescribeTexture(const uint32_t fetch[6]);

// ---- The per-draw binding ----------------------------------------------------------------------------
// The sampler, as VkSamplerCreateInfo's fields (numeric, vk:: above).
struct SamplerDescription
{
    uint32_t magFilter = vk::kFilterNearest, minFilter = vk::kFilterNearest, mipmapMode = vk::kMipmapModeNearest;
    uint32_t addressModeU = vk::kAddressClampToEdge, addressModeV = vk::kAddressClampToEdge, addressModeW = vk::kAddressClampToEdge;
    bool anisotropyEnable = false;
    float maxAnisotropy = 1.0f;
    float minLod = 0.0f, maxLod = vk::kLodClampNone;
    uint32_t borderColor = vk::kBorderFloatTransparentBlack;
    bool operator==(const SamplerDescription&) const = default;
};

// The fetch instruction's overrides (ShaderTranslation::Filters in shader_glsl.h: 3 = the fetch
// constant's) and the dimension the shader samples with (1 2D, 2 3D, 3 cube).
struct ShaderFetch
{
    uint32_t mag = 3, min = 3, mip = 3, aniso = 7;
    uint32_t dimension = 1;
};

struct TextureBinding
{
    bool ok = false;
    std::string error;
    // What the shader sees in x, y, z, w (VkComponentMapping values: vk::kSwizzleR..A, Zero, One),
    // the fetch swizzle folded through the host format's swizzle (texture_cache.cc GuestToHostSwizzle).
    uint32_t components[4] = {vk::kSwizzleR, vk::kSwizzleG, vk::kSwizzleB, vk::kSwizzleA};
    bool gamma = false;        // XE_TEXTURE_<slot>_GAMMA: x, y, z after the swizzle are all sign 3
    int32_t expAdjust = 0;     // XE_TEXTURE_<slot>_SCALE ldexp(1.0, expAdjust) when not 0
    float lodBias = 0.0f;      // XE_TEXTURE_<slot>_LOD_BIAS, in LODs (the field / 32)
    SamplerDescription sampler;
};

// The per-draw binding of `fetch` for a draw whose shader fetches it as `shader` says, given the
// host format its (already accepted) storage uses. Refuses: a shader dimension that is not the constant's (2D
// fetch of a 2D constant, 3D fetch of a 3D constant),
// integer number format, signed or biased components, mixed gamma, gamma on w, swizzle values 6-7,
// clamp modes Xenia only approximates, YCbCr border colours, an unresolved or invalid filter.
TextureBinding DescribeBinding(const uint32_t fetch[6], const HostFormat& host, const ShaderFetch& shader);

}  // namespace runtime::video
