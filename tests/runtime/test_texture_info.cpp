// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// T0 tests of the texture cache's CPU side (M4 unit 7): the fetch-constant decode and description
// (texture_info.h), the untile and endian rules (texture_untile.h) and the write-watch cache model
// (texture_cache.h). No Vulkan header, no device. Expected values come from Xenia 67f16c4e315f's
// source (the line numbers in each comment) by hand, cross-checked with the transcription
// the local capture oracle_texture_layout (not distributed) (its output: the local capture oracle_table (not distributed)), and from
// ForzaRecomp's OBSERVED textures of FM4's first screens: the intro video's three linear k_8 planes (Y 1280x720, U and V 640x360), k_8_8_8_8
// (formats 6 and 50) with a swizzle, and a tiled DXT1 8in16 single-level texture (its size is not
// recorded there; 1280x720 is the shape tested). "CONTROL" names a negative control: a wrong
// behaviour that must not happen, or a refusal that must.
//
//   tests/run_texture_tests.sh
#include "texture_cache.h"
#include "texture_info.h"
#include "texture_untile.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace runtime::video;

namespace {
int failures = 0;
void Expect(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "pass" : "FAIL", name);
    failures += !ok;
}
bool Has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// Fetch constants, hand-encoded from xenos.h:1206-1307 (comments give the field placement).
// Dword 0: type bits 0-1, sign x/y/z/w bits 2-9, clamp x/y/z bits 10-18, pitch >> 5 bits 22-30, tiled bit 31.
// Dword 1: format bits 0-5, endian 6-7, stacked bit 10, base address >> 12 bits 12-31.
// Dword 2: width - 1 bits 0-12, height - 1 bits 13-25.
// Dword 3: num format bit 0, swizzle bits 1-12, exp adjust 13-18, mag 19-20, min 21-22, mip 23-24, aniso 25-27.
// Dword 4: mip min level bits 2-5, mip max level 6-9, lod bias 12-21.
// Dword 5: border colour bits 0-1, dimension 9-10, packed mips bit 11, mip address >> 12 bits 12-31.
constexpr uint32_t kClampEdgeXYZ = 2u << 10 | 2u << 13 | 2u << 16;  // 0x24800
constexpr uint32_t kSwizzleXYZW = 0x688u << 1;                        // 0xD10
constexpr uint32_t kLinearMagMin = 1u << 19 | 1u << 21;              // 0x280000
constexpr uint32_t kMipBaseMap = 2u << 23;                            // 0x1000000
constexpr uint32_t kDim2D = 1u << 9;                                  // 0x200

// The intro video's Y plane as ForzaRecomp observed it: linear k_8 1280x720, pitch 1280 (40 << 22 =
// 0x0A000000), swizzle XXXX (0), linear mag/min, base-map mip, clamp to edge, at physical 0x01000000.
const uint32_t kYPlane[6] = {2u | kClampEdgeXYZ | 40u << 22, 2u | 0x01000000u, 1279u | 719u << 13, kLinearMagMin | kMipBaseMap, 0, kDim2D};
// The U plane: 640x360, pitch 640 (20 << 22), at 0x01100000.
const uint32_t kUPlane[6] = {2u | kClampEdgeXYZ | 20u << 22, 2u | 0x01100000u, 639u | 359u << 13, kLinearMagMin | kMipBaseMap, 0, kDim2D};
// A tiled DXT1 (18), endian 8in16 (1 << 6), 1280x720, pitch 1280, XYZW, at 0x02000000.
const uint32_t kDxt1[6] = {2u | kClampEdgeXYZ | 40u << 22 | 1u << 31, 18u | 1u << 6 | 0x02000000u, 1279u | 719u << 13,
                           kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};
// A tiled DXT1 256x256 with packed mips 0..8 (mip max level 8 << 6) at 0x03000000, mips at 0x03100000.
const uint32_t kDxt1Mips[6] = {2u | kClampEdgeXYZ | 8u << 22 | 1u << 31, 18u | 0x03000000u, 255u | 255u << 13,
                               kSwizzleXYZW | kLinearMagMin | 1u << 23, 8u << 6, kDim2D | 1u << 11 | 0x03100000u};
// A linear k_8_8_8_8 (6) 64x64, pitch 64 (2 << 22), swizzle ZYX1 (2 | 1 << 3 | 0 << 6 | 5 << 9 = 0xA0A), at 0x04000000.
const uint32_t kRgba8Zyx1[6] = {2u | kClampEdgeXYZ | 2u << 22, 6u | 0x04000000u, 63u | 63u << 13, 0xA0Au << 1 | kLinearMagMin | kMipBaseMap, 0,
                                kDim2D};

std::vector<uint32_t> With(const uint32_t base[6], int dword, uint32_t clear, uint32_t set)
{
    std::vector<uint32_t> v(base, base + 6);
    v[dword] = (v[dword] & ~clear) | set;
    return v;
}
std::vector<uint32_t> With(const std::vector<uint32_t>& base, int dword, uint32_t clear, uint32_t set) { return With(base.data(), dword, clear, set); }
const ShaderFetch kFromFetch{};  // every filter from the fetch constant, 2D

// The per-draw binding of a fetch constant: its storage first (a refused storage is reported as the
// binding's error), then DescribeBinding with that storage's host format.
TextureBinding Bind(const uint32_t* fetch, const ShaderFetch& shader = kFromFetch)
{
    const TextureDescription d = DescribeTexture(fetch);
    if (!d.ok)
    {
        TextureBinding b;
        b.error = d.error;
        return b;
    }
    return DescribeBinding(fetch, d.host, shader);
}

// ---- texture_info: decode ------------------------------------------------------------------------
void TestDecode()
{
    // Every field at a distinct value, placed by hand from xenos.h:1206-1307.
    const uint32_t d[6] = {
        2u | 3u << 2 | 0u << 4 | 1u << 6 | 2u << 8 | 6u << 10 | 1u << 13 | 4u << 16 | 1u << 19 | 44u << 22 | 1u << 31,  // dword 0
        18u | 2u << 6 | 1u << 8 | 1u << 10 | 1u << 11 | 0x1F000000u,                                                 // dword 1
        1279u | 719u << 13 | 5u << 26,                                                                               // dword 2
        1u | 0xA0Au << 1 | 0x20u << 13 | 1u << 19 | 0u << 21 | 2u << 23 | 3u << 25 | 5u << 28 | 1u << 31,            // dword 3
        1u | 1u << 1 | 1u << 2 | 9u << 6 | 1u << 10 | 1u << 11 | 0x3FFu << 12 | 0x1Fu << 22 | 0x10u << 27,            // dword 4
        1u | 1u << 2 | 1u << 3 | 0xFu << 5 | 3u << 9 | 1u << 11 | 0x1E000000u,                                       // dword 5
    };
    const TextureFetch f = DecodeTextureFetch(d);
    Expect(f.type == 2 && f.sign[0] == 3 && f.sign[1] == 0 && f.sign[2] == 1 && f.sign[3] == 2, "decode: type and the four signs (dword 0 bits 0-9)");
    Expect(f.clamp[0] == 6 && f.clamp[1] == 1 && f.clamp[2] == 4 && f.signedRfMode == 1, "decode: clamp x/y/z and signed rf mode (bits 10-19)");
    Expect(f.pitchTexels == 44 * 32 && f.tiled, "decode: pitch 44 << 5 = 1408 texels (bits 22-30), tiled (bit 31)");
    Expect(f.format == 18 && f.endian == 2 && f.requestSize == 1 && f.stacked && f.nearestClampPolicy == 1 && f.baseAddress == 0x1F000000u,
           "decode: format, endian, request size, stacked, clamp policy, base address (dword 1)");
    Expect(f.width == 1280 && f.height == 720 && f.stackDepth == 5, "decode: 2D size 1280x720 (+1), stack depth 5 (dword 2)");
    // Dword 2 = 0x1459E4FF read as size_3d: width bits 0-10 = 0x4FF + 1 = 1280; height bits 11-21 =
    // (0x1459E4FF >> 11 = 0x28B3C) & 0x7FF = 0x33C + 1 = 829; depth bits 22-31 = 0x51 + 1 = 82.
    Expect(f.width3D == 1280 && f.height3D == 829 && f.depth3D == 82, "decode: the same dword read as a 3D size (11/11/10 bits)");
    Expect(f.numFormat == 1 && f.swizzle == 0xA0A && f.expAdjust == -32 && f.magFilter == 1 && f.minFilter == 0 && f.mipFilter == 2 &&
               f.anisoFilter == 3 && f.arbitraryFilter == 5 && f.borderSize == 1,
           "decode: num format, swizzle ZYX1, exp adjust 0x20 = -32 (6-bit two's complement), filters, aniso, border size (dword 3)");
    Expect(f.volMagFilter == 1 && f.volMinFilter == 1 && f.mipMinLevel == 1 && f.mipMaxLevel == 9 && f.lodBias == -1,
           "decode: volume filters, mip min 1 / max 9, lod bias 0x3FF = -1 (10-bit two's complement) (dword 4)");
    Expect(f.borderColor == 1 && f.dimension == 3 && f.packedMips && f.mipAddress == 0x1E000000u, "decode: border colour, dimension cube, packed, mip address (dword 5)");
    const uint32_t zero[6] = {};
    const TextureFetch z = DecodeTextureFetch(zero);
    Expect(z.width == 1 && z.height == 1 && z.expAdjust == 0 && z.lodBias == 0 && !z.tiled && z.pitchTexels == 0, "decode: all-zero dwords are a 1x1 texture with zero bias");
}

// ---- texture_info: formats and the first screens' textures ---------------------------------------
void TestFormatsAndDescriptions()
{
    // texture_info_formats.inl: k_8 1x1x8, k_DXT1 4x4x4 (8 bytes a block), k_DXT4_5 4x4x8 (16), k_8_8_8_8 32,
    // k_Cr_Y1_Cb_Y0_REP 2x1x16 (4), k_32_32_32_FLOAT 96 bits (12 bytes), k_2_10_10_10_FLOAT_EDRAM 32.
    Expect(BytesPerBlock(GetFormatInfo(2)) == 1 && BytesPerBlock(GetFormatInfo(18)) == 8 && BytesPerBlock(GetFormatInfo(20)) == 16 &&
               BytesPerBlock(GetFormatInfo(6)) == 4 && BytesPerBlock(GetFormatInfo(11)) == 4 && BytesPerBlock(GetFormatInfo(57)) == 12 &&
               BytesPerBlock(GetFormatInfo(63)) == 4 && !std::strcmp(GetFormatInfo(54).name, "k_2_10_10_10_AS_16_16_16_16"),
           "formats: bytes per block and names from texture_info_formats.inl");
    HostFormat h;
    Expect(HostFormatFor(2, h) && h.vkFormat == vk::kFormatR8Unorm && h.formatSwizzle == kSwizzleRRRR && !h.blockCompressed, "host format: k_8 -> R8_UNORM, RRRR (vulkan_texture_cache.cc:100-104)");
    Expect(HostFormatFor(18, h) && h.vkFormat == vk::kFormatBc1RgbaUnormBlock && h.blockCompressed && HostFormatFor(51, h) && h.vkFormat == vk::kFormatBc1RgbaUnormBlock,
           "host format: k_DXT1 and k_DXT1_AS_16_16_16_16 -> BC1_RGBA_UNORM_BLOCK (:186-190, :340-344)");
    Expect(HostFormatFor(6, h) && h.vkFormat == vk::kFormatR8G8B8A8Unorm && HostFormatFor(50, h) && h.vkFormat == vk::kFormatR8G8B8A8Unorm &&
               HostFormatFor(54, h) && h.vkFormat == vk::kFormatA2B10G10R10UnormPack32,
           "host format: k_8_8_8_8 / _AS_16_16_16_16 -> R8G8B8A8_UNORM, k_2_10_10_10_AS_16_16_16_16 -> A2B10G10R10 (:121-125, :335-339, :355-360)");
    Expect(HostFormatFor(20, h) && h.vkFormat == 137 && h.blockCompressed && HostFormatFor(53, h) && h.vkFormat == 137,
           "host format: k_DXT4_5 and k_DXT4_5_AS_16_16_16_16 -> BC3_UNORM_BLOCK (137; :196-200, :350-354)");
    Expect(HostFormatFor(10, h) && h.vkFormat == 16 && h.vkFormatSigned == 17 && h.formatSwizzle == kSwizzleRGGG && !h.blockCompressed &&
               !h.floatData && BytesPerBlock(GetFormatInfo(10)) == 2,
           "host format: k_8_8 -> R8G8_UNORM (16), signed R8G8_SNORM (17), RGGG, 2-byte blocks (:141-145; texture_info_formats.inl:11)");
    Expect(HostFormatFor(49, h) && h.vkFormat == 141 && h.formatSwizzle == kSwizzleRGGG && h.blockCompressed && h.vkFormatSigned == 0 &&
               BytesPerBlock(GetFormatInfo(49)) == 16,
           "host format: k_DXN -> BC5_UNORM (141), RGGG, no signed form, 16-byte 4x4 blocks (:330-333)");
    Expect(HostFormatFor(19, h) && h.vkFormat == 135 && h.blockCompressed, "host format: k_DXT2_3 -> BC2_UNORM (135) (:191-195)");
    Expect(!HostFormatFor(4, h) && !HostFormatFor(7, h) && h.vkFormat == vk::kFormatUndefined,
           "CONTROL: 5_6_5 and 2_10_10_10 have no host mapping yet");

    // The Y plane: level 0 only, extent 1280 * 719 + 1280 = 921,600 (texture_util.cc:420-423: rows - 1
    // pitches plus the last row; the base's pitch is the fetch constant's, 1280, not re-aligned).
    TextureDescription y = DescribeTexture(kYPlane);
    Expect(y.ok && y.width == 1280 && y.height == 720 && y.levelCount == 1 && y.log2BytesPerBlock == 0 && !y.fetch.tiled,
           "Y plane: described, 1280x720, one level, 1-byte blocks, linear");
    Expect(y.base.physical == 0x01000000u && y.base.size == 921600 && y.mips.size == 0, "Y plane: watch range = base + 921,600 bytes, no mips");
    Expect(y.levels[0].rowPitchBytes == 1280 && y.levels[0].pitchBlocks == 1280 && y.levels[0].widthBlocks == 1280 && y.levels[0].heightBlocks == 720 &&
               y.levels[0].extentBytes == 921600 && y.levels[0].x == 0 && !y.levels[0].packed,
           "Y plane: level 0 pitch 1280 bytes, 1280x720 blocks, extent 921,600");
    const TextureBinding yb = Bind(kYPlane);
    Expect(yb.ok && yb.components[0] == vk::kSwizzleR && yb.components[1] == vk::kSwizzleR && yb.components[2] == vk::kSwizzleR && yb.components[3] == vk::kSwizzleR,
           "Y plane: swizzle XXXX through R8's RRRR -> the shader sees R in x, y, z, w");
    Expect(!yb.gamma && yb.expAdjust == 0 && yb.lodBias == 0.0f, "Y plane: no gamma, no scale, no bias");
    Expect(yb.sampler.magFilter == vk::kFilterLinear && yb.sampler.minFilter == vk::kFilterLinear && yb.sampler.mipmapMode == vk::kMipmapModeNearest &&
               yb.sampler.maxLod == 0.25f && yb.sampler.minLod == 0.0f,
           "Y plane: linear mag/min, base-map mip -> mipmap nearest with maxLod 0.25 (vulkan_texture_cache.cc:804-807)");
    Expect(yb.sampler.addressModeU == vk::kAddressClampToEdge && yb.sampler.addressModeV == vk::kAddressClampToEdge && yb.sampler.addressModeW == vk::kAddressClampToEdge &&
               !yb.sampler.anisotropyEnable && yb.sampler.borderColor == vk::kBorderFloatTransparentBlack,
           "Y plane: clamp to edge on u, v (and w for 2D: texture_util.cc:601-611), no anisotropy, black border unused");
    TextureDescription u = DescribeTexture(kUPlane);
    Expect(u.ok && u.base.size == 640 * 359 + 640 && u.base.size == 230400, "U plane: 640x360 linear k_8 extent 230,400");

    // The tiled DXT1: blocks 320x180, pitch 320 blocks; the last block (319, 179) is in the tile at
    // (288, 160): macro = (9 + 5 * 10) << 10 = 60416 = 0xEC00; (0xEC00 & ~0x1FF) << 3 = 0x76000 = 483328;
    // the other terms are 0; + (0x400 << 3) = 8192 -> 491,520 = 320 * 192 * 8 (texture_util.cc:490-517).
    TextureDescription x = DescribeTexture(kDxt1);
    Expect(x.ok && x.host.vkFormat == vk::kFormatBc1RgbaUnormBlock && x.log2BytesPerBlock == 3 && x.fetch.endian == 1 && x.fetch.tiled,
           "DXT1: described as BC1, 8-byte blocks, endian 8in16, tiled");
    Expect(x.levels[0].pitchBlocks == 320 && x.levels[0].widthBlocks == 320 && x.levels[0].heightBlocks == 180 && x.base.size == 491520 && x.mips.size == 0,
           "DXT1 1280x720: pitch 320 blocks, 320x180 blocks, extent 491,520");
    const TextureBinding xb = Bind(kDxt1);
    Expect(xb.ok && xb.components[0] == vk::kSwizzleR && xb.components[1] == vk::kSwizzleG && xb.components[2] == vk::kSwizzleB && xb.components[3] == vk::kSwizzleA,
           "DXT1: swizzle XYZW -> R G B A");

    // k_8_8_8_8 ZYX1: x reads host B (guest Z -> RGBA swizzle's component 2), y G, z R, w one.
    TextureDescription r = DescribeTexture(kRgba8Zyx1);
    const TextureBinding rb = Bind(kRgba8Zyx1);
    Expect(r.ok && rb.ok && rb.components[0] == vk::kSwizzleB && rb.components[1] == vk::kSwizzleG && rb.components[2] == vk::kSwizzleR && rb.components[3] == vk::kSwizzleOne,
           "k_8_8_8_8 ZYX1: components B G R One (GuestToHostSwizzle through RGBA)");
    Expect(r.base.size == 256 * 63 + 256 && r.base.size == 16384 && r.levels[0].rowPitchBytes == 256, "k_8_8_8_8 64x64 linear: row pitch 256, extent 16,384");

    // Gamma: signs x, y, z = 3 (dword 0 bits 2-7 = 0xFC) with XYZW -> XE_TEXTURE_<slot>_GAMMA.
    const TextureBinding g = Bind(With(kRgba8Zyx1, 0, 0xFFu << 2, 0x3Fu << 2).data());
    Expect(g.ok && g.gamma, "gamma: x, y, z all sign 3 -> gamma (w unsigned)");
    // Gamma with a constant among x, y, z: swizzle Z Y 1 W (2 | 1 << 3 | 5 << 6 | 3 << 9 = 0x74A) reads
    // gamma sources 2 and 1 and the constant one: still gamma (0 and 1 are fixed points of the curve);
    // w reads source 3, unsigned.
    Expect(Bind(With(With(kRgba8Zyx1, 0, 0xFFu << 2, 0x3Fu << 2), 3, 0xFFFu << 1, 0x74Au << 1).data()).gamma,
           "gamma: a constant 1 among gamma x, y, z keeps gamma");
    // exp adjust 3 (bits 13-18) and lod bias -16 (0x3F0 at bits 12-21: -16 / 32 = -0.5).
    const TextureBinding b = Bind(With(With(kRgba8Zyx1, 3, 0x3Fu << 13, 3u << 13), 4, 0x3FFu << 12, 0x3F0u << 12).data());
    Expect(b.ok && b.expAdjust == 3 && b.lodBias == -0.5f, "exp adjust 3 and lod bias -16/32 = -0.5 reach the binding");
    // The binding is the per-draw part: the same storage key with another swizzle yields the same
    // TextureKey and storage but another component mapping (texture_cache.h Find's contract).
    const std::vector<uint32_t> xyz1 = With(kRgba8Zyx1, 3, 0xFFFu << 1, 0xA88u << 1);
    Expect(KeyOf(DecodeTextureFetch(xyz1.data())) == KeyOf(DecodeTextureFetch(kRgba8Zyx1)) && Bind(xyz1.data()).components[0] == vk::kSwizzleR,
           "binding: another swizzle on the same storage = the same key, another component mapping");
}

// ---- texture_info: mips ----------------------------------------------------------------------------
void TestMipLayout()
{
    // 256x256 DXT1 tiled, packed mips, mip max 8. Levels: size max = log2_floor(256) = 8; mip address set,
    // min 0, max min(8, 8) = 8 -> 9 levels (texture_util.cc:60-92). Packed level = log2_ceil(256) - 4 = 4
    // (texture_util.h:36-39). Mip storage k (texture_util.cc:310-344): pitch max(256 >> k, 1) texels ->
    // blocks aligned to 32: k = 1: 32 blocks, 256 bytes; rows 32; stride align(8192, 4096) = 8192;
    // k = 2, 3, 4: 16, 8, 4 blocks -> 32 aligned: the same 8192. Level 1 at mip + 0, level 2 at + 8192,
    // level 3 at + 16384, levels 4-8 in the tail at + 24576 (the tail is stored as level 4).
    // Packed offsets (texture_util.cc:144-181, square: the "taller" branch, x first): level 4: x 16 ->
    // block 4; 5: x 8 -> 2; 6: x 4 -> 1; 7: packed 3: y = (1 << (8 - 4)) >> 1 = 8 -> block 2; 8: y 16 >> 2 = 4 -> 1.
    // Extents: level 0: 64x64 blocks at pitch 64: tile (32, 32): macro = (1 + 1 * 2) << 10 = 3072, << 3 =
    // 24576, + 8192 = 32768. Each tail level lies in the first tile: 0 + 8192. Mips range: max over
    // levels of offset + extent = 24576 + 8192 = 32768.
    TextureDescription d = DescribeTexture(kDxt1Mips);
    Expect(d.ok && d.levelCount == 9 && d.packedLevel == 4 && d.levels.size() == 9, "256x256 DXT1 packed: 9 levels, packed level 4");
    Expect(d.levels[0].address == 0x03000000u && d.levels[0].pitchBlocks == 64 && d.levels[0].extentBytes == 32768, "level 0: base, pitch 64 blocks, extent 32,768");
    Expect(d.levels[1].address == 0x03100000u && d.levels[2].address == 0x03100000u + 8192 && d.levels[3].address == 0x03100000u + 16384,
           "levels 1-3 at mip + 0, 8192, 16384 (4 KB-aligned strides)");
    Expect(d.levels[1].pitchBlocks == 32 && d.levels[1].widthBlocks == 32 && d.levels[1].heightBlocks == 32 && d.levels[1].extentBytes == 8192 && !d.levels[1].packed,
           "level 1: 32x32 blocks at pitch 32, extent 8192, not packed");
    bool tail = true;
    for (uint32_t l = 4; l <= 8; l++)
        tail = tail && d.levels[l].address == 0x03100000u + 24576 && d.levels[l].packed && d.levels[l].pitchBlocks == 32 && d.levels[l].extentBytes == 8192;
    Expect(tail, "levels 4-8 share the tail at mip + 24576 (pitch 32 blocks, extent 8192)");
    Expect(d.levels[4].x == 4 && d.levels[4].y == 0 && d.levels[5].x == 2 && d.levels[6].x == 1 && d.levels[7].x == 0 && d.levels[7].y == 2 && d.levels[8].y == 1,
           "packed offsets in blocks: 4 -> (4, 0), 5 -> (2, 0), 6 -> (1, 0), 7 -> (0, 2), 8 -> (0, 1)");
    Expect(d.levels[4].widthBlocks == 4 && d.levels[6].widthBlocks == 1 && d.levels[8].widthBlocks == 1 && d.levels[8].widthTexels == 1,
           "level sizes: 16x16 texels = 4 blocks, 4x4 = 1, 1x1 = 1 block");
    Expect(d.base.physical == 0x03000000u && d.base.size == 32768 && d.mips.physical == 0x03100000u && d.mips.size == 32768,
           "watch ranges: base 32,768 bytes, mips 32,768 bytes (mips_total_extent)");
    // Mip max 2 limits the levels; mip min 1 is refused; mip address 0 means one level even with max 8.
    Expect(DescribeTexture(With(kDxt1Mips, 4, 0xFu << 6, 2u << 6).data()).levelCount == 3, "mip max level 2 -> 3 levels");
    TextureDescription m = DescribeTexture(With(kDxt1Mips, 4, 0xFu << 2, 1u << 2).data());
    Expect(!m.ok && Has(m.error, "first mip level 1"), "CONTROL: mip min level 1 refused (Xenia would ignore the base)");
    Expect(DescribeTexture(With(kDxt1Mips, 5, 0xFFFFF000u, 0).data()).levelCount == 1, "mip address 0 -> one level (texture_util.cc:74-76)");
    // 48x32 linear DXT1, pitch 64, packed, max 5 (log2_floor(48) = 5): packed level log2_ceil(32) - 4 = 1;
    // the base 12x8 blocks, pitch 64 texels -> 16 blocks -> 32 aligned = 256 bytes: extent 256 * 7 + 96 =
    // 1888; level 1 in the tail at mip + 0 at (0, 16) texels -> (0, 4) blocks, 6x4 blocks, pitch
    // next_pow2(48) >> 1 = 32 texels -> 8 blocks -> 32 aligned -> 256 bytes (linear: 256-aligned too):
    // extent 256 * 4 + 256 * 3 + 48 = 1840; the mips range is the max over the tail = 1840.
    const uint32_t k48[6] = {2u | kClampEdgeXYZ | 2u << 22, 18u | 0x05000000u, 47u | 31u << 13, kSwizzleXYZW | kLinearMagMin | 1u << 23, 5u << 6,
                             kDim2D | 1u << 11 | 0x05100000u};
    TextureDescription n = DescribeTexture(k48);
    Expect(n.ok && n.levelCount == 6 && n.packedLevel == 1 && n.base.size == 1888 && n.mips.size == 1840 && n.levels[1].y == 4 && n.levels[1].rowPitchBytes == 256 &&
               n.levels[4].x == 4 && n.levels[5].x == 2,
           "48x32 linear DXT1 packed: base 1888, mips 1840, level 1 at row block 4, levels 4/5 at x 4/2");
}

// ---- texture_info: refusals --------------------------------------------------------------------------
void TestRefusals()
{
    struct Case { const char* name; std::vector<uint32_t> fetch; ShaderFetch shader; const char* reason; };
    const std::vector<Case> cases = {
        {"type 3 (vertex)", With(kYPlane, 0, 3u, 3u), {}, "not a texture"},
        {"type 0 (invalid)", With(kYPlane, 0, 3u, 0u), {}, "not a texture"},
        {"dimension 3D", With(kYPlane, 5, 3u << 9, 2u << 9), {}, "constant's dimension is 2"},  // a 3D constant fetched as 2D (refused at binding)
        {"dimension cube", With(kYPlane, 5, 3u << 9, 3u << 9), {}, "stack depth is 0"},  // a cube needs six faces (TestCube in test_texture_vk.cpp)
        // FM4 Recomp: 1D constants are described (a height-1 image); size_1d is 24 bits, so the Y plane's dword 2 reads as a 5,891,328-texel
        // row (0x0059E4FF + 1), past Xenia's 8192 limit (texture_cache.cc:902-907); a tiled 1D is refused like Xenia's (:908-914).
        {"dimension 1D wider than 8192", With(kYPlane, 5, 3u << 9, 0u), {}, "5891328 texels wide"},
        {"dimension 1D tiled", With(With(kYPlane, 5, 3u << 9, 0u), 0, 0, 1u << 31), {}, "1D texture that is tiled"},
        {"stacked", With(kYPlane, 1, 1u << 10, 1u << 10), {}, "stacked"},
        {"shader samples 3D", std::vector<uint32_t>(kYPlane, kYPlane + 6), ShaderFetch{3, 3, 3, 7, 2}, "dimension 2"},
        {"format 5_6_5 (4)", With(kYPlane, 1, 0x3Fu, 4u), {}, "no host format"},
        {"format 5_6_5 (4)", With(kYPlane, 1, 0x3Fu, 4u), {}, "no host format"},
        {"integer number format", With(kYPlane, 3, 1u, 1u), {}, "integer"},
        {"base address 0", With(kYPlane, 1, 0xFFFFF000u, 0u), {}, "base address 0"},
        {"base past 512 MB", With(kYPlane, 1, 0xFFFFF000u, 0x20000000u), {}, "past the 512 MB"},
        {"k_8 with endian 8in16", With(kYPlane, 1, 3u << 6, 1u << 6), {}, "1-byte blocks"},
        {"DXT1 with endian 16in32 (8-byte blocks take every endian)", With(kDxt1, 1, 3u << 6, 3u << 6), {}, nullptr},
        {"signed component", With(kYPlane, 0, 3u << 2, 1u << 2), {}, "signed component 0"},
        {"biased component (the shader's v * 2 - 1)", With(kYPlane, 0, 3u << 2, 2u << 2), {}, nullptr},
        {"mixed gamma (x only)", With(kRgba8Zyx1, 0, 0xFFu << 2, 3u << 2), {}, "mixed gamma"},
        {"gamma on w (XYZW, all four signs 3)", With(With(kRgba8Zyx1, 0, 0xFFu << 2, 0xFFu << 2), 3, 0xFFFu << 1, kSwizzleXYZW), {}, "gamma on the w"},
        {"swizzle value 6", With(kRgba8Zyx1, 3, 0xFFFu << 1, 6u << 1), {}, "undefined value 6"},
        {"swizzle value 7 in w", With(kRgba8Zyx1, 3, 0xFFFu << 1, 7u << 10), {}, "undefined value 7"},
        {"clamp mirror-clamp-to-edge (3)", With(kYPlane, 0, 7u << 10, 3u << 10), {}, "clamp mode 3"},
        {"clamp to halfway (4) on y", With(kYPlane, 0, 7u << 13, 4u << 13), {}, "clamp mode 4 on axis 1"},
        {"clamp mirror-clamp-to-border (7)", With(kYPlane, 0, 7u << 10, 7u << 10), {}, "clamp mode 7"},
        {"border colour YCbCr (2) with clamp to border", With(With(kYPlane, 0, 7u << 10, 6u << 10), 5, 3u, 2u), {}, "border colour 2"},
        {"mag filter 2 from the fetch constant", With(kYPlane, 3, 3u << 19, 2u << 19), {}, "mag filter 2"},
        {"min filter 3 from the fetch constant (unresolved)", With(kYPlane, 3, 3u << 21, 3u << 21), {}, "min filter 3"},
        {"mip filter 3 from the fetch constant (unresolved)", With(kYPlane, 3, 3u << 23, 3u << 23), {}, "mip filter 3"},
        {"aniso 7 from the fetch constant (unresolved)", With(kYPlane, 3, 7u << 25, 7u << 25), {}, "anisotropic filter 7"},
        {"aniso 6 (undefined)", With(kYPlane, 3, 7u << 25, 6u << 25), {}, "anisotropic filter 6"},
        {"mip address past 512 MB", With(With(kDxt1Mips, 5, 0xFFFFF000u, 0x20000000u), 4, 0, 0), {}, "mip address past"},
        {"level wider than its pitch (pitch 32 texels for 1280)", With(kYPlane, 0, 0x1FFu << 22, 1u << 22), {}, "wider than its pitch"},
        {"level ends past the physical memory", With(kYPlane, 1, 0xFFFFF000u, 0x1FFF0000u), {}, "ends past the physical memory"},
    };
    for (const Case& c : cases)
    {
        const TextureBinding d = Bind(c.fetch.data(), c.shader);
        if (c.reason == nullptr)
            Expect(d.ok, (std::string("accepted: ") + c.name).c_str());
        else
            Expect(!d.ok && Has(d.error, c.reason), (std::string("CONTROL refused: ") + c.name + " -> \"" + d.error + "\"").c_str());
    }
    // Instruction overrides: point mag from the instruction over a linear fetch constant; the
    // instruction's mip filter linear -> mipmap linear and maxLod unclamped; aniso 3 (4:1) from the fetch
    // constant -> enabled, 4 (vulkan_texture_cache.cc:797-802). Xenia's Vulkan sampler uses mag for min
    // and mip too (:766-772); here min and mip are their own fields (D3D semantics; a deliberate difference).
    const TextureBinding s = Bind(With(kYPlane, 3, 7u << 25, 3u << 25).data(), ShaderFetch{0, 3, 1, 7, 1});
    Expect(s.ok && s.sampler.magFilter == vk::kFilterNearest && s.sampler.minFilter == vk::kFilterLinear && s.sampler.mipmapMode == vk::kMipmapModeLinear &&
               s.sampler.maxLod == vk::kLodClampNone && s.sampler.anisotropyEnable && s.sampler.maxAnisotropy == 4.0f,
           "sampler: instruction point mag over fetch linear; instruction linear mip; fetch aniso 4:1");
    const TextureBinding w = Bind(With(With(kYPlane, 0, 7u << 10 | 7u << 13, 0u << 10 | 1u << 13), 5, 3u, 1u).data());
    Expect(w.ok && w.sampler.addressModeU == vk::kAddressRepeat && w.sampler.addressModeV == vk::kAddressMirroredRepeat && w.sampler.borderColor == vk::kBorderFloatTransparentBlack,
           "sampler: repeat / mirrored repeat; the border colour is ignored unless a clamp uses the border (:624-631)");
    const TextureBinding bw = Bind(With(With(kYPlane, 0, 7u << 10, 6u << 10), 5, 3u, 1u).data());
    Expect(bw.ok && bw.sampler.addressModeU == vk::kAddressClampToBorder && bw.sampler.borderColor == vk::kBorderFloatOpaqueWhite,
           "sampler: clamp to border with colour 1 -> opaque white (:811-818)");
}

// ---- texture_untile --------------------------------------------------------------------------------
void TestTiledOffsets()
{
    // Hand values from texture_util.cc:443-458 (pitch 32 unless noted). Worked examples:
    //  log2 0 (1, 0): micro 1, offset 1 -> 1.  (0, 1): offset (1 << 4) = 16 -> 16.
    //  log2 0 (8, 0): x & 7 = 0, micro 0, offset 0 -> ((8 >> 3) & 3) << 6 = 64.
    //  log2 0 (0, 8): micro (8 << 2) = 32, offset (32 & ~0xF) << 1 = 64 -> (64 & 0x1C0) << 2 = 256, + (((8 & 8) >> 2) & 3) << 6 = 128 -> 384.
    //  log2 0 (31, 31): micro 7 + 56 = 63, offset (48 << 1) + 15 + 16 = 127 -> (16 << 7) 2048 + (64 << 2) 256 + ((2 + 3) & 3) << 6 = 64 + 63 -> 2431.
    //  log2 1 (31, 31): micro 126, offset 224 + 14 + 16 = 254 -> 2048 + (192 << 2) 768 + 64 + 62 -> 2942.
    //  log2 3 (0, 8): micro 32 << 3 = 256, offset 512 -> (512 << 3) 4096 + 128 -> 4224.
    //  log2 3 (31, 31): micro 504, offset 992 + 8 + 16 = 1016 -> (512 << 3) 4096 + 2048 + (448 << 2) 1792 + 64 + 56 -> 8056.
    //  log2 4 (31, 31): micro 1008, offset 2016 + 16 = 2032 -> (1536 << 3) 12288 + 2048 + 1792 + 64 + 48 -> 16240.
    //  log2 3 (32, 0) pitch 64: macro 1 << 10 = 1024 -> (1024 & ~0x1FF) << 3 = 8192 (one 32x32x8 tile).
    //  log2 2 (0, 32) pitch 1000: Xenia aligns the pitch to 1024 (:446): macro 32 << 9 = 16384 -> << 3 = 131072
    //  (front_buffer.h's unaligned form gives 126976: reference/gpu/front_buffer.md, FALSIFIED).
    struct Row { uint32_t log2, x, y, pitch, expected; };
    const Row rows[] = {
        {0, 0, 0, 32, 0}, {0, 1, 0, 32, 1}, {0, 0, 1, 32, 16}, {0, 2, 0, 32, 2}, {0, 0, 2, 32, 8}, {0, 8, 0, 32, 64}, {0, 0, 8, 32, 384},
        {0, 31, 31, 32, 2431}, {0, 32, 0, 64, 512}, {0, 0, 32, 64, 1024},
        {1, 1, 0, 32, 2}, {1, 0, 1, 32, 16}, {1, 0, 2, 32, 32}, {1, 8, 0, 32, 64}, {1, 0, 8, 32, 640}, {1, 31, 31, 32, 2942}, {1, 32, 0, 64, 1024}, {1, 0, 32, 64, 4096},
        {2, 1, 0, 32, 4}, {2, 0, 1, 32, 16}, {2, 31, 0, 32, 236}, {2, 0, 31, 32, 3984}, {2, 31, 31, 32, 3964}, {2, 0, 8, 32, 1152}, {2, 32, 0, 64, 4096},
        {2, 0, 32, 1000, 131072}, {2, 999, 32, 1000, 258092},
        {3, 1, 0, 32, 8}, {3, 2, 0, 32, 32}, {3, 0, 1, 32, 16}, {3, 0, 2, 32, 512}, {3, 8, 0, 32, 64}, {3, 0, 8, 32, 4224}, {3, 31, 31, 32, 8056},
        {3, 32, 0, 64, 8192}, {3, 0, 32, 64, 16384},
        {4, 1, 0, 32, 32}, {4, 2, 0, 32, 256}, {4, 0, 1, 32, 16}, {4, 0, 2, 32, 1024}, {4, 0, 8, 32, 8320}, {4, 31, 31, 32, 16240}, {4, 32, 0, 64, 16384},
    };
    bool all = true;
    for (const Row& r : rows)
        if (TiledOffset2D(r.x, r.y, r.pitch, r.log2) != r.expected)
        {
            all = false;
            std::printf("  TiledOffset2D(%u, %u, %u, log2 %u) = %u, expected %u\n", r.x, r.y, r.pitch, r.log2, TiledOffset2D(r.x, r.y, r.pitch, r.log2), r.expected);
        }
    Expect(all, "TiledOffset2D: 44 hand values at 1, 2, 4, 8 and 16 bytes per block (texture_util.cc:443-458)");
    // Whole tiles at 4 bytes and more are permutations of [0, 1024 << log2) in block steps; at 1 and 2
    // bytes tiles interleave (texture_util.h:219-239), so only the weighted digest is checked there.
    const uint64_t digests[5] = {958141440ull, 1116743680ull, 1430556672ull, 2726838272ull, 5252190208ull};  // oracle_table.txt
    for (uint32_t log2 = 0; log2 < 5; log2++)
    {
        uint64_t digest = 0;
        std::vector<bool> seen(1024, false);
        bool permutation = true;
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 32; x++)
            {
                const uint32_t o = TiledOffset2D(x, y, 32, log2);
                digest += uint64_t(o) * (1 + x + 32 * y);
                const uint32_t slot = o >> log2;
                permutation = permutation && (o & ((1u << log2) - 1)) == 0 && slot < 1024 && !seen[slot];
                if (slot < 1024)
                    seen[slot] = true;
            }
        char name[128];
        std::snprintf(name, sizeof name, "TiledOffset2D: first tile at log2 %u: weighted digest %llu%s", log2, (unsigned long long)digests[log2],
                      log2 >= 2 ? " and a permutation of the tile" : "");
        Expect(digest == digests[log2] && (log2 < 2 || permutation), name);
    }
    // Upper bounds (:490-517): 32x32 at pitch 32: 0 + 0xA00, 0xC00, 0x1000, 0x2000, 0x4000; the DXT1 1280x720 case.
    Expect(TiledAddressUpperBound2D(32, 32, 32, 0) == 2560 && TiledAddressUpperBound2D(32, 32, 32, 1) == 3072 && TiledAddressUpperBound2D(32, 32, 32, 2) == 4096 &&
               TiledAddressUpperBound2D(32, 32, 32, 3) == 8192 && TiledAddressUpperBound2D(32, 32, 32, 4) == 16384 && TiledAddressUpperBound2D(320, 180, 320, 3) == 491520 &&
               TiledAddressUpperBound2D(8, 4, 32, 3) == 8192 && TiledAddressUpperBound2D(0, 4, 32, 3) == 0,
           "TiledAddressUpperBound2D: 0xA00 / 0xC00 below 4 bytes, 0x400 << log2 above; 1280x720 DXT1 = 491,520; empty = 0");
    Expect(TiledPitchBlocks(1280, 4) == 320 && TiledPitchBlocks(1280, 1) == 1280 && TiledPitchBlocks(48, 4) == 32 && TiledPitchBlocks(100, 4) == 32,
           "TiledPitchBlocks: ceil(pitch / block width) aligned to 32 (texture_util.cc:315-318)");
    Expect(LinearExtent(1280, 720, 1280, 0) == 921600 && LinearExtent(12, 8, 256, 3) == 1888 && LinearExtent(0, 8, 256, 3) == 0,
           "LinearExtent: (rows - 1) * pitch + last row (:420-423)");
}

void TestEndian()
{
    // xenos.h:1070-1088: 8in16 swaps the bytes of each half, 8in32 reverses the four bytes, 16in32 swaps the halves.
    Expect(EndianSwap32(0x11223344u, 0) == 0x11223344u && EndianSwap32(0x11223344u, 1) == 0x22114433u && EndianSwap32(0x11223344u, 2) == 0x44332211u &&
               EndianSwap32(0x11223344u, 3) == 0x33441122u,
           "EndianSwap32: none, 8in16, 8in32, 16in32 on 0x11223344");
    // endian.xesli:25-31: XeEndianSwap16 only acts on 8in16.
    Expect(EndianSwap16Pair(0x11223344u, 1) == 0x22114433u && EndianSwap16Pair(0x11223344u, 2) == 0x11223344u && EndianSwap16Pair(0x11223344u, 3) == 0x11223344u,
           "EndianSwap16Pair: 8in16 swaps, 8in32 and 16in32 do nothing");
    Expect(EndianSupported(0, 0) && !EndianSupported(0, 1) && EndianSupported(1, 1) && !EndianSupported(1, 2) && !EndianSupported(1, 3) && EndianSupported(2, 3) &&
               EndianSupported(3, 2) && EndianSupported(4, 1) && !EndianSupported(5, 0),
           "EndianSupported: 1-byte only 0, 2-byte 0-1, 4-16 bytes 0-3, nothing above 16");
    // A DXT1 block in guest memory under 8in16: two big-endian 5:6:5 endpoints then four index bytes;
    // XeEndianSwap32 on each dword (texture_load_64bpb.xesli:41-43) = bytes 0<->1, 2<->3, 4<->5, 6<->7:
    // the endpoints become little-endian uint16s (what BC1 wants) and the index bytes pair-swap.
    uint8_t dxt1[8] = {0xA0, 0xA1, 0xB0, 0xB1, 0xC0, 0xC1, 0xC2, 0xC3};
    SwapBlockEndian(dxt1, 3, 1);
    const uint8_t dxt1Expected[8] = {0xA1, 0xA0, 0xB1, 0xB0, 0xC1, 0xC0, 0xC3, 0xC2};
    Expect(!std::memcmp(dxt1, dxt1Expected, 8), "SwapBlockEndian: DXT1 block under 8in16 pair-swaps every byte pair");
    uint8_t q[4] = {0x11, 0x22, 0x33, 0x44};
    SwapBlockEndian(q, 2, 2);
    Expect(q[0] == 0x44 && q[1] == 0x33 && q[2] == 0x22 && q[3] == 0x11, "SwapBlockEndian: a 4-byte block under 8in32 is reversed");
    uint8_t h[4] = {0x11, 0x22, 0x33, 0x44};
    SwapBlockEndian(h, 2, 3);
    Expect(h[0] == 0x33 && h[1] == 0x44 && h[2] == 0x11 && h[3] == 0x22, "SwapBlockEndian: a 4-byte block under 16in32 swaps the halves");
    uint8_t s[2] = {0x12, 0x34};
    SwapBlockEndian(s, 1, 1);
    Expect(s[0] == 0x34 && s[1] == 0x12, "SwapBlockEndian: a 2-byte block under 8in16 swaps its bytes");
    uint8_t n[2] = {0x12, 0x34};
    SwapBlockEndian(n, 1, 0);
    Expect(n[0] == 0x12 && n[1] == 0x34, "CONTROL: endian 0 changes nothing");
}

void TestCopies()
{
    // A tiled k_8 32x32 tile (pitch 32) filled with its own offsets' low byte: detiling an 8x8 region
    // at (0, 0) must place tiled[TiledOffset2D(x, y)] at out[y * 8 + x]; hand offsets for (1, 0) = 1,
    // (0, 1) = 16, (8, 0) is outside the region, (0, 2) = 8, (7, 7): micro 7 + 24 = 31, offset (16 << 1) +
    // 15 + 16 = 63 -> ((0 + 0) & 3) << 6 = 0, + 63 -> 63.
    std::vector<uint8_t> tiled(2560);
    for (size_t i = 0; i < tiled.size(); i++)
        tiled[i] = uint8_t(i * 7 + 3);
    uint8_t out[64];
    Expect(DetileBlocksAt(tiled.data(), tiled.size(), out, 0, 0, 8, 8, 32, 0, 0), "DetileBlocksAt: k_8 8x8 region of a 32-wide tile");
    Expect(out[0] == tiled[0] && out[1] == tiled[1] && out[8] == tiled[16] && out[16] == tiled[8] && out[63] == tiled[63] && out[7] == tiled[7],
           "DetileBlocksAt: (1, 0) <- 1, (0, 1) <- 16, (0, 2) <- 8, (7, 7) <- 63");
    // A DXT1 tail: the 16x16-texel level 4 of the 256x256 texture is 4x4 blocks at block (4, 0): block (4, 0)
    // is at TiledOffset2D(4, 0, 32, 3): micro 4 << 3 = 32, offset (32 & ~0xF) << 1 = 64 -> (64 & 0x1C0) << 2 = 256.
    std::vector<uint8_t> tail(8192);
    for (size_t i = 0; i < tail.size(); i++)
        tail[i] = uint8_t(i ^ (i >> 8));
    uint8_t blocks[4 * 4 * 8];
    Expect(DetileBlocksAt(tail.data(), tail.size(), blocks, 4, 0, 4, 4, 32, 3, 0) && !std::memcmp(blocks, tail.data() + 256, 8) &&
               !std::memcmp(blocks + 8, tail.data() + TiledOffset2D(5, 0, 32, 3), 8),
           "DetileBlocksAt: a packed level at block (4, 0): first block from offset 256");
    uint8_t swapped[4 * 4 * 8];
    Expect(DetileBlocksAt(tail.data(), tail.size(), swapped, 4, 0, 4, 4, 32, 3, 1) && swapped[0] == tail[257] && swapped[1] == tail[256] && swapped[7] == tail[262],
           "DetileBlocksAt: endian 8in16 applied per block after the copy");
    Expect(!DetileBlocksAt(tail.data(), 8191, blocks, 4, 0, 4, 4, 32, 3, 0), "CONTROL: a source one byte short of the extent is refused");
    Expect(!DetileBlocksAt(tail.data(), tail.size(), blocks, 4, 0, 4, 4, 48, 3, 0), "CONTROL: a pitch that is not a multiple of 32 blocks is refused");
    Expect(!DetileBlocksAt(tail.data(), tail.size(), blocks, 30, 0, 4, 4, 32, 3, 0), "CONTROL: a region past the pitch is refused");
    Expect(!DetileBlocksAt(tiled.data(), tiled.size(), out, 0, 0, 8, 8, 32, 0, 1), "CONTROL: k_8 with endian 8in16 is refused");
    // Linear: the Y plane's rows are the source's rows; a region at (x0, y0) starts at y0 * pitch + x0.
    std::vector<uint8_t> plane(921600);
    for (size_t i = 0; i < plane.size(); i++)
        plane[i] = uint8_t(i * 13 + 1);
    std::vector<uint8_t> rows(1280 * 2);
    Expect(CopyLinearBlocksAt(plane.data(), plane.size(), rows.data(), 0, 718, 1280, 2, 1280, 0, 0) && !std::memcmp(rows.data(), plane.data() + 718 * 1280, 2560),
           "CopyLinearBlocksAt: the last two rows of the Y plane");
    uint8_t sub[6 * 4 * 8];  // 6x4 blocks of 8 bytes; row 1 starts at byte 48
    std::vector<uint8_t> lin(1840);
    for (size_t i = 0; i < lin.size(); i++)
        lin[i] = uint8_t(i);
    Expect(CopyLinearBlocksAt(lin.data(), lin.size(), sub, 0, 4, 6, 4, 256, 3, 0) && !std::memcmp(sub, lin.data() + 4 * 256, 48) && sub[48] == lin[5 * 256],
           "CopyLinearBlocksAt: the 48x32 texture's level 1 at row block 4 (6x4 blocks)");
    Expect(!CopyLinearBlocksAt(lin.data(), 1839, sub, 0, 4, 6, 4, 256, 3, 0), "CONTROL: a linear source one byte short is refused");
    Expect(!CopyLinearBlocksAt(lin.data(), lin.size(), sub, 0, 0, 33, 1, 256, 3, 0), "CONTROL: a linear row wider than its pitch is refused");
}

// ---- texture_cache --------------------------------------------------------------------------------
struct FakeBackend final : WatchBackend
{
    struct Call { uint32_t physical, size; bool readOnly; };
    std::vector<Call> calls;
    bool fail = false;
    std::vector<bool> readOnlyHostPage = std::vector<bool>(kGuestPageCount * kGuestPageSize / 16384, false);
    bool Protect(uint32_t physical, uint32_t size, bool readOnly) override
    {
        calls.push_back({physical, size, readOnly});
        if (fail)
            return false;
        for (uint32_t h = physical / 16384; h <= (physical + size - 1) / 16384; h++)
            readOnlyHostPage[h] = readOnly;
        return true;
    }
};

void TestCache()
{
    FakeBackend fake;
    WriteWatch watch(fake);
    TextureCache cache(watch);
    Expect(watch.Current() == 0 && watch.Epoch() == 0 && watch.WriteSequence(0x01000000u, 921600) == 0, "watch: fresh sequences are 0");

    // First sight of the Y plane: described, no host image -> load.
    TextureCache::Lookup a = cache.Find(kYPlane);
    Expect(a.entry && a.load && a.error.empty() && cache.Size() == 1 && cache.GetStats().described == 1, "cache: a new key is described and needs a load");
    const LoadPlan plan = cache.Plan(*a.entry);
    Expect(plan.stagingBytes == 921600 && plan.levelOffsets.size() == 1 && plan.levelBytes[0] == 921600 && plan.sequence == 0, "cache: plan = one level of 921,600 bytes at sequence 0");
    Expect(cache.Loaded(*a.entry, plan) && a.entry->uploaded && a.entry->generation == 1, "cache: Loaded marks the entry valid at the plan's sequence");
    // 921,600 bytes from 0x01000000 span host pages 1024 .. 1024 + 56 (921599 / 16384 = 56): 57 pages, one Protect of 57 * 16384.
    Expect(fake.calls.size() == 1 && fake.calls[0].physical == 0x01000000u && fake.calls[0].size == 57 * 16384 && fake.calls[0].readOnly && watch.GetStats().protects == 57,
           "watch: Loaded protects the 57 host pages behind the Y plane (ceil(921600 / 16384)) in one call");
    Expect(watch.IsWatchedHostPage(0x01000000u) && watch.IsWatchedHostPage(0x01000000u + 921599) && !watch.IsWatchedHostPage(0x01000000u + 57 * 16384) && !watch.IsWatchedHostPage(0x00FFFFFFu),
           "watch: the first and last host pages are watched, their neighbours are not");

    // Second and third lookups: valid; one page walk (Loaded forces it), then none.
    TextureCache::Lookup b = cache.Find(kYPlane);
    TextureCache::Lookup c = cache.Find(kYPlane);
    Expect(b.entry == a.entry && !b.load && !c.load && cache.GetStats().walks == 1 && cache.GetStats().described == 1,
           "cache: a loaded entry is valid; the first check walks its pages, the next is an epoch compare only");

    // A guest write inside the plane: the fault handler asks the watch; the host page is unprotected,
    // the sequences move, the next lookup finds the entry stale.
    const size_t before = fake.calls.size();
    Expect(watch.OnWriteFault(0x01000000u + 5000), "watch: a write fault on a watched page is taken");
    Expect(fake.calls.size() == before + 1 && fake.calls.back().physical == 0x01000000u && fake.calls.back().size == 16384 && !fake.calls.back().readOnly,
           "watch: the fault unprotects exactly its 16 KB host page");
    Expect(watch.Current() == 1 && watch.Epoch() == 1 && watch.WriteSequence(0x01000000u, 16384) == 1 && watch.WriteSequence(0x01000000u + 16384, 4096) == 0 &&
               watch.WriteSequence(0x01000000u + 12288, 4096) == 1,
           "watch: all four guest pages of the host page carry sequence 1; the next host page's do not (ReCarbon write_watch.h:9-10)");
    Expect(!watch.IsWatchedHostPage(0x01000000u + 5000) && watch.IsWatchedHostPage(0x01000000u + 16384), "watch: the faulted host page is no longer watched, the rest stays");
    TextureCache::Lookup d = cache.Find(kYPlane);
    Expect(d.entry == a.entry && d.load && cache.GetStats().stale == 1 && cache.GetStats().walks == 2 && !a.entry->uploaded, "cache: the written texture is stale -> load again");
    Expect(watch.GetStats().faults == 1 && watch.GetStats().unprotects == 1, "watch: stats count the fault and the unprotect");

    // A reload: Current is taken before the copy (sequence 1), then the whole range is watched again.
    const LoadPlan plan2 = cache.Plan(*a.entry);
    Expect(plan2.sequence == 1 && cache.Loaded(*a.entry, plan2) && a.entry->generation == 2 && !cache.Find(kYPlane).load,
           "cache: reloaded at sequence 1 (its host image is a new generation), valid again");
    Expect(fake.calls.back().size == 57 * 16384 && watch.IsWatchedHostPage(0x01000000u + 5000), "watch: the reload protects the faulted page again");
    // A write that lands between Current() and Watch() (during the untile) still invalidates: the
    // sequence it moves is above the plan's.
    TextureCache::Lookup e0 = cache.Find(kYPlane);
    (void)e0;
    watch.OnWriteFault(0x01000000u + 900000);
    const LoadPlan plan3 = cache.Plan(*a.entry);  // sequence 2
    cache.Loaded(*a.entry, plan3);
    Expect(!cache.Find(kYPlane).load, "cache: a load after the write is valid");
    cache.Find(kYPlane);
    const LoadPlan plan4 = cache.Plan(*a.entry);  // sequence 2, taken before this write...
    watch.OnWriteFault(0x01000000u + 900000);    // ... which moves the page to 3 while the copy runs
    cache.Loaded(*a.entry, plan4);
    Expect(cache.Find(kYPlane).load, "cache: a write during the untile (after Plan, before Loaded) leaves the entry stale");

    // Writes outside: not the watch's business, nothing moves, no unprotect, the entry stays valid.
    cache.Loaded(*a.entry, cache.Plan(*a.entry));
    cache.Find(kYPlane);
    const size_t calls = fake.calls.size();
    const uint64_t epoch = watch.Epoch();
    Expect(!watch.OnWriteFault(0x05000000u) && !watch.OnWriteFault(0x01000000u + 57 * 16384) && !watch.OnWriteFault(0xFFFFFFFFu) && fake.calls.size() == calls && watch.Epoch() == epoch,
           "CONTROL: a fault outside every watched page is not taken (falls through to the guest fault stop); nothing moves");
    // Walks so far: the first check after each of the five Loaded calls, plus the stale check after the first fault = 6.
    Expect(!cache.Find(kYPlane).load && cache.GetStats().walks == 6, "CONTROL: the entry stays valid with no page walk");
    // The padding of the last host page is watched too (false sharing by design): a write there invalidates.
    Expect(watch.OnWriteFault(0x01000000u + 921600 + 100) && cache.Find(kYPlane).load, "watch: a write in the last host page's tail (past the texture) still invalidates (false sharing, documented)");

    // A GPU write (resolve) into the range: MarkWritten, no fault.
    cache.Loaded(*a.entry, cache.Plan(*a.entry));
    cache.Find(kYPlane);
    watch.MarkWritten(0x01000000u + 500000, 4);
    Expect(watch.GetStats().marked == 1 && cache.Find(kYPlane).load && watch.IsWatchedHostPage(0x01000000u + 500000),
           "watch: a GPU write (MarkWritten) invalidates and leaves the page watched against the CPU");
    watch.MarkWritten(0x01000000u + 921600, 4);
    cache.Loaded(*a.entry, cache.Plan(*a.entry));
    cache.Find(kYPlane);
    watch.MarkWritten(0x01000000u + 921600, 4);  // the byte just past the texture: not in its 4 KB pages? 921600 = 225 * 4096 exactly: the next guest page
    Expect(!cache.Find(kYPlane).load, "CONTROL: a GPU write to the guest page just past the texture does not invalidate (sequences are per 4 KB page)");

    // Two textures, two keys; a second fetch constant with the same storage but another swizzle shares the entry.
    TextureCache::Lookup u = cache.Find(kUPlane);
    Expect(u.entry && u.entry != a.entry && u.load && cache.Size() == 2, "cache: the U plane is a second entry");
    const std::vector<uint32_t> ySwizzled = With(kYPlane, 3, 0xFFFu << 1, 0xA88u << 1);  // XYZ1
    Expect(cache.Find(ySwizzled.data()).entry == a.entry && cache.Size() == 2, "cache: the same storage with another swizzle is the same entry (sampler/swizzle are per draw)");
    // The heap recomputes a host page's access (a neighbouring commit): the fake loses the protection;
    // IsWatchedHostPage still says watched (the heap's mask), and Rearm restores the protection.
    cache.Loaded(*u.entry, cache.Plan(*u.entry));
    fake.readOnlyHostPage[(0x01100000u + 8192) / 16384] = false;  // what SetHostAccess would do without the mask
    Expect(watch.IsWatchedHostPage(0x01100000u + 8192), "heap coexistence: the watch still reports the page watched after a heap recompute");
    Expect(watch.Rearm(0x01100000u, 230400) && fake.readOnlyHostPage[(0x01100000u + 8192) / 16384] && watch.GetStats().rearms == 15,
           "heap coexistence: Rearm protects the U plane's 15 watched host pages again (ceil(230400 / 16384))");
    Expect(watch.Rearm(0x09000000u, 65536) && watch.GetStats().rearms == 15, "CONTROL: Rearm over unwatched pages protects nothing");

    // A refused fetch constant: no entry, the reason; a failing backend: Loaded false, the entry invalid.
    TextureCache::Lookup bad = cache.Find(With(kYPlane, 1, 0x3Fu, 4u).data());  // 5_6_5: no host format
    Expect(!bad.entry && Has(bad.error, "no host format") && cache.GetStats().refused == 1 && cache.Size() == 2, "CONTROL: a refused fetch constant makes no entry and names its reason");
    fake.fail = true;
    TextureCache::Lookup v = cache.Find(kUPlane);
    Expect(v.entry && !cache.Loaded(*v.entry, cache.Plan(*v.entry)) && !v.entry->uploaded && cache.Find(kUPlane).load,
           "CONTROL: a watch the backend cannot take leaves the entry invalid (never drawn unwatched)");
    fake.fail = false;

    // The mip texture's plan: tight levels, 16-byte aligned offsets: 32768, 8192, 2048, 512, 128, 32, 8, 8, 8
    // bytes at 0, 32768, 40960, 43008, 43520, 43648, 43680, 43696, 43712; total 43728.
    TextureCache::Lookup m = cache.Find(kDxt1Mips);
    const LoadPlan mp = cache.Plan(*m.entry);
    Expect(mp.levelBytes.size() == 9 && mp.levelBytes[0] == 32768 && mp.levelBytes[1] == 8192 && mp.levelBytes[6] == 8 && mp.levelOffsets[1] == 32768 &&
               mp.levelOffsets[7] == 43696 && mp.levelOffsets[8] == 43712 && mp.stagingBytes == 43728,
           "cache: the 256x256 DXT1 plan packs nine levels into 43,728 staging bytes");
    Expect(cache.Loaded(*m.entry, mp) && fake.calls[fake.calls.size() - 2].size == 2 * 16384 && fake.calls.back().physical == 0x03100000u && fake.calls.back().size == 2 * 16384,
           "cache: Loaded watches the base (32 KB = 2 host pages) and the mips (2 host pages) separately");
    Expect(watch.OnWriteFault(0x03100000u + 30000) && cache.Find(kDxt1Mips).load, "cache: a write into the mip tail invalidates the texture");

    // The per-frame bound of the video planes (rule 6 of texture_cache.h): Y 57 + U 15 + V 15 host pages.
    Expect(57 == (921600 + 16383) / 16384 && 15 == (230400 + 16383) / 16384, "bound: a rewritten video frame costs 57 + 15 + 15 = 87 faults, unprotects and protects");
}

}  // namespace

// FM4 Recomp (FM4_WRITE_WATCH): the orderings the real backend depends on, by hand on host pages of 16 KB.
struct OrderBackend final : WatchBackend
{
    WriteWatch* watch = nullptr;
    bool fail = false;
    std::vector<std::string> log;  // "ro <page> watched=<flag at the call>" / "rw <page> watched=<flag>"
    bool Protect(uint32_t physical, uint32_t size, bool readOnly) override
    {
        for (uint32_t h = physical / 16384; h <= (physical + size - 1) / 16384; h++)
            log.push_back(std::string(readOnly ? "ro " : "rw ") + std::to_string(h) + " watched=" + (watch->IsWatchedHostPage(h * 16384) ? "1" : "0"));
        return !fail;
    }
};

void TestWatchOrderings()
{
    OrderBackend b;
    WriteWatch w(b);
    b.watch = &w;
    // Watch: the flags are set before the protection (a store faulting in between finds its page watched).
    Expect(w.Watch(0x02000000u, 20000) && b.log.size() == 2 && b.log[0] == "ro 2048 watched=1" && b.log[1] == "ro 2049 watched=1",
           "watch order: 20,000 bytes at 0x02000000 = host pages 2048, 2049, flagged before the protect call");
    // A refused protection takes the flags back.
    b.fail = true;
    Expect(!w.Watch(0x02100000u, 16384) && !w.IsWatchedHostPage(0x02100000u), "watch order: a refused protect leaves the page unwatched");
    b.fail = false;
    b.log.clear();
    // The hook's path: writable first (the flag still set at the call), then the sequence moves, then the flag clears; an unwatched
    // page the heap means writable is handled the same (a concurrent second fault, or a heap recompute racing Watch).
    const uint64_t before = w.Current();
    Expect(w.OnProtectionFault(0x02000000u + 100) && b.log.size() == 1 && b.log[0] == "rw 2048 watched=1" && !w.IsWatchedHostPage(0x02000000u) &&
               w.WriteSequence(0x02000000u, 16384) == before + 1 && w.WriteSequence(0x02004000u, 16384) == 0,
           "fault: unprotect with the flag still set, then host page 2048's four guest pages at sequence 1, page 2049 untouched");
    Expect(w.OnProtectionFault(0x02000000u + 200) && b.log.size() == 2 && b.log[1] == "rw 2048 watched=0" && w.WriteSequence(0x02000000u, 4096) == before + 2,
           "fault: a second fault on the same page (already unwatched) is still taken and still moves the sequence");
    Expect(!w.OnProtectionFault(0x20000000u), "CONTROL: an address past the 512 MB is not taken");
    // The heap changes a watched page's access: unwatched, counted as written, no protect call (the heap sets the access).
    b.log.clear();
    w.HostAccessChanged(0x02004000u, 16384);
    Expect(b.log.empty() && !w.IsWatchedHostPage(0x02004000u) && w.WriteSequence(0x02004000u, 16384) == before + 3,
           "heap access change: page 2049 unwatched and written, no backend call");
    // A system call is about to write: the watched pages it covers are made writable and unwatched first; unwatched ones untouched.
    Expect(w.Watch(0x03000000u, 32768), "re-watch two pages for the host write");
    b.log.clear();
    w.BeforeHostWrite(0x03000000u + 100, 20000);
    Expect(b.log.size() == 2 && b.log[0] == "rw 3072 watched=0" && b.log[1] == "rw 3073 watched=0" && w.WriteSequence(0x03000000u, 32768) == before + 4,
           "host write: pages 3072 and 3073 unprotected (flag exchanged first) and written");
    b.log.clear();
    w.BeforeHostWrite(0x03000000u, 100);
    Expect(b.log.empty(), "CONTROL: a host write over unwatched pages calls nothing");
}

int main()
{
    TestDecode();
    TestFormatsAndDescriptions();
    TestMipLayout();
    TestRefusals();
    TestTiledOffsets();
    TestEndian();
    TestCopies();
    TestCache();
    TestWatchOrderings();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
