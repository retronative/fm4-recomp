// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 FM4 Recomp contributors
//
// T0 tests of the texture unit's binding path, CPU side (M4 unit 7 on the renderer: texture_upload.h): the
// staging placement in the slot's upload ring, the staging bytes of a tiled DXT1 and a linear k_8 texture,
// the sampler and view keys of the fetch constants OBSERVED on our disc (the local capture trace11 (not distributed); the
// texture_NOTES.md addendum), the validity policy and the GPU-write (resolve) log. No Vulkan header, no
// device. Expected values are hand-computed: tiled offsets from Xenia xenia-canary 67f16c4e315f
// src/xenia/gpu/texture_util.cc:443-458 (GetTiledOffset2D), the 8in16 swap from xenos.h:1070-1088 and
// shaders/endian.xesli:37-46, the fetch-constant fields from xenos.h:1206-1307 (worked in the comments),
// never from this code's output. "CONTROL" names a negative control.
//
//   tests/run_texture_vk_tests.sh
#include "pipeline_key.h"
#include "texture_upload.h"
#include "edram.h"
#include "edram_vk_plan.h"
#include "resolve_resident.h"
#include <algorithm>
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

// The OBSERVED fetch constants (trace11 `tex` lines). Hand decode (xenos.h:1206-1307):
//  UI      (1<<31 | 0A0048FE) 11A8F073 0059E4FF 01280D10 00000003 00000A00: dword 0 type 2, signs x y z w = 3 3 3 0 (bits 2-9 of
//          0x48FE = 0x3F), clamp x y = 2 (bits 10-15 of 0x48FE: 010 010), pitch 0x28 << 5 = 1280, tiled; dword 1 format
//          0x33 = 51 (DXT1_AS_16_16_16_16), endian (0x73 >> 6) = 1 (8in16), base 11A8F000; dword 2 1280x720; dword 3
//          swizzle (0xD10 >> 1) = 0x688 = X Y Z W, mag bit 19 = 1, min bits 21-22 = 1, mip bits 23-24 = 2 (base map);
//          dword 5 dimension (0xA00 >> 9) & 3 = 1 (2D), mip address 0 (one level).
//  COMP    (1<<31 | 0A0248FE) 13F2B0B2 0059E4FF 01000C14 00000000 00000200: as UI but format 0x32 = 50, endian (0xB2 >> 6) & 3 = 2
//          (8in32), base 13F2B000; dword 3 swizzle (0xC14 >> 1) = 0x60A = Z Y X W, mag 0, min 0, mip 2.
//  Y       0A024802 0C180002 0059E4FF 01281400 00000003 00000200: signs 0, clamp 2 2, pitch 1280, linear; k_8 (2),
//          endian 0, base 0C180000; 1280x720; swizzle (0x1400 >> 1) = 0xA00 = X X X 1, mag 1, min 1, mip 2.
//  U       06024802 0C261002 002CE27F 01281400 00000003 00000200: pitch 0x18 << 5 = 768, k_8 at 0C261000, 640x360
//          (0x27F + 1, (0x2CE27F >> 13) + 1 = 0x168), as Y otherwise.
const uint32_t kUi[6] = {1u << 31 | 0x0A0048FEu, 0x11A8F073u, 0x0059E4FFu, 0x01280D10u, 0x00000003u, 0x00000A00u};
// The HDR resolve's destination as the next draws sample it (the local capture draw_c2d1 (not distributed) draw 27039 tex 0).
const uint32_t kHdr[6] = {1u << 31 | 0x0A004802u, 0x1BB710B6u, 0x0059E4FFu, 0x01282C14u, 0x00000003u, 0x00000200u};
// The 32x32 k_32_FLOAT texture of the HDR luminance pass (the local capture draw_loop1 (not distributed) draw 22725 tex 1).
const uint32_t kLum[6] = {1u << 31 | 0x00404956u, 0x08D4F0A4u, 0x0003E01Fu, 0x012816D1u, 0x00000003u, 0x00000200u};
// The 3D lookup texture (the local capture draw_tex3d1 (not distributed) draw 33248 slot 7, printed by the stop).
const uint32_t kLut[6] = {0x00824802u, 0x11A6E086u, 0x03C0780Fu, 0x00A80C14u, 0x00000003u, 0x00000400u};
const uint32_t kComp[6] = {1u << 31 | 0x0A0248FEu, 0x13F2B0B2u, 0x0059E4FFu, 0x01000C14u, 0x00000000u, 0x00000200u};
const uint32_t kY[6] = {0x0A024802u, 0x0C180002u, 0x0059E4FFu, 0x01281400u, 0x00000003u, 0x00000200u};
const uint32_t kU[6] = {0x06024802u, 0x0C261002u, 0x002CE27Fu, 0x01281400u, 0x00000003u, 0x00000200u};

// Synthetic constants (field placement as test_texture_info.cpp).
constexpr uint32_t kClampEdgeXYZ = 2u << 10 | 2u << 13 | 2u << 16;
constexpr uint32_t kSwizzleXYZW = 0x688u << 1;
constexpr uint32_t kLinearMagMin = 1u << 19 | 1u << 21;
constexpr uint32_t kMipBaseMap = 2u << 23;
constexpr uint32_t kDim2D = 1u << 9;
constexpr uint32_t kSmallBase = 0x00100000u;
// A tiled DXT1_AS_16_16_16_16 16x8 texels (4x2 blocks), endian 8in16, pitch 32 texels (1 << 22: 8 blocks,
// aligned to 32), at 0x00100000.
const uint32_t kDxt1Small[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 51u | 1u << 6 | kSmallBase, 15u | 7u << 13,
                                kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};
const uint32_t kDxt1SmallNoSwap[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 51u | kSmallBase, 15u | 7u << 13,
                                      kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};
// A linear k_8 5x2, pitch 32 texels (1 << 22), at 0x00100000.
const uint32_t kK8Small[6] = {2u | kClampEdgeXYZ | 1u << 22, 2u | kSmallBase, 4u | 1u << 13, kLinearMagMin | kMipBaseMap, 0, kDim2D};
// Mapped by texture_info.cpp but outside the observed set: k_8_8_8_8 (6), 64x64 linear.
// Format 18 (k_DXT1): mapped (texture_info.cpp), observed in FM4.
const uint32_t kDxt1Format18[6] = {2u | kClampEdgeXYZ | 2u << 22, 18u | 0x04000000u, 63u | 63u << 13, kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};
const uint32_t kRgba8Format6[6] = {2u | kClampEdgeXYZ | 2u << 22, 6u | 0x04000000u, 63u | 63u << 13, kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};
// A tiled DXT1_AS_16_16_16_16 256x256 with mips 0..8 at 0x03000000 (mips at 0x03100000).
const uint32_t kDxt1Mips[6] = {2u | kClampEdgeXYZ | 8u << 22 | 1u << 31, 51u | 0x03000000u, 255u | 255u << 13,
                               kSwizzleXYZW | kLinearMagMin | 1u << 23, 8u << 6, kDim2D | 1u << 11 | 0x03100000u};

const TextureDeviceFeatures kFull{true, true};

// Guest memory for FillStaging: `bytes` at physical `physical`, byte i = i & 0xFF.
struct FakeGuest
{
    uint32_t physical;
    std::vector<uint8_t> bytes;
    bool refuse = false;
};
const uint8_t* GuestOf(void* context, uint32_t physical, uint32_t size, std::string& error)
{
    const FakeGuest& g = *static_cast<const FakeGuest*>(context);
    if (g.refuse || physical < g.physical || uint64_t(physical) + size > uint64_t(g.physical) + g.bytes.size())
    {
        error = "guest range not in one view (fake)";
        return nullptr;
    }
    return g.bytes.data() + (physical - g.physical);
}
FakeGuest Pattern(uint32_t physical, size_t size)
{
    FakeGuest g{physical, std::vector<uint8_t>(size)};
    for (size_t i = 0; i < size; i++)
        g.bytes[i] = uint8_t(i & 0xFF);
    return g;
}

bool Components(const TextureBinding& b, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    return b.components[0] == x && b.components[1] == y && b.components[2] == z && b.components[3] == w;
}

void TestStagingPlacement()
{
    Expect(StagingAlignment(8, 1) == 16 && StagingAlignment(1, 1) == 16 && StagingAlignment(4, 64) == 64 && StagingAlignment(16, 4) == 16,
           "staging alignment = max(16, block bytes, optimalBufferCopyOffsetAlignment)");
    Expect(StagingAlignment(8, 3) == 0 && StagingAlignment(8, 0) == 0 && StagingAlignment(0, 1) == 0,
           "CONTROL: a non-power-of-two (or zero) alignment yields 0 (refused)");
    // UploadRing(4096, uniform 256, storage 16): a 4-byte uniform block at 0 (used 4); then 64 staging bytes at a
    // multiple of 64: the ring gives 127 bytes at 16 (storage alignment), used 16 + 127 = 143; the block is at 64.
    UploadRing ring(4096, 256, 16);
    uint64_t uniform = 0, offset = 0;
    Expect(ring.Allocate(4, true, uniform) && uniform == 0 && ring.Used() == 4, "ring: a 4-byte uniform block at 0");
    Expect(PlaceInRing(ring, 64, 64, offset) && offset == 64 && ring.Used() == 143 && offset + 64 <= ring.Used(),
           "ring: 64 staging bytes placed at 64 (raw 16, aligned up), inside the 127 allocated");
    Expect(!PlaceInRing(ring, 4096, 16, offset) && ring.Used() == 143, "CONTROL: a block the ring cannot hold is refused and allocates nothing");
    Expect(!PlaceInRing(ring, 16, 48, offset) && !PlaceInRing(ring, 16, 0, offset) && ring.Used() == 143,
           "CONTROL: a non-power-of-two alignment is refused before allocating");
}

void TestObservedConstants()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const ShaderFetch shader{};  // filters 3 = the fetch constant's; 2D
    const PreparedTexture ui = m.Prepare(kUi, shader, true, kFull);
    Expect(ui.entry != nullptr && ui.load, "UI DXT1 (51) 1280x720 tiled 8in16 at 11A8F000 is accepted and loads");
    if (ui.entry == nullptr)
    {
        std::printf("  %s\n", ui.error.c_str());
        return;
    }
    Expect(ui.plan.stagingBytes == 460800 && ui.plan.levelOffsets.size() == 1 && ui.plan.levelOffsets[0] == 0,
           "UI staging: 320 x 180 blocks x 8 bytes = 460,800, one level at 0");
    Expect(Components(ui.binding, vk::kSwizzleR, vk::kSwizzleG, vk::kSwizzleB, vk::kSwizzleA) && IdentityComponents(ui.binding.components) && ui.binding.gamma,
           "UI view: X Y Z W through RGBA = R G B A (identity), gamma (signs 3 3 3 0)");
    const SamplerDescription& us = ui.binding.sampler;
    Expect(us.magFilter == vk::kFilterLinear && us.minFilter == vk::kFilterLinear && us.mipmapMode == vk::kMipmapModeNearest &&
               us.addressModeU == vk::kAddressClampToEdge && us.addressModeV == vk::kAddressClampToEdge && us.addressModeW == vk::kAddressClampToEdge &&
               !us.anisotropyEnable && us.minLod == 0.0f && us.maxLod == 0.25f && us.borderColor == vk::kBorderFloatTransparentBlack,
           "UI sampler: linear/linear, base map (mip nearest, maxLod 0.25), clamp to edge");

    const PreparedTexture comp = m.Prepare(kComp, shader, true, kFull);
    Expect(comp.entry != nullptr && comp.plan.stagingBytes == 3686400, "composite k_8_8_8_8 (50) 1280x720 8in32 at 13F2B000: 1280 x 720 x 4 = 3,686,400 bytes");
    Expect(Components(comp.binding, vk::kSwizzleB, vk::kSwizzleG, vk::kSwizzleR, vk::kSwizzleA) && !IdentityComponents(comp.binding.components) &&
               comp.binding.gamma,
           "composite view: Z Y X W = B G R A (needs imageViewFormatSwizzle), gamma");
    const SamplerDescription& cs = comp.binding.sampler;
    Expect(cs.magFilter == vk::kFilterNearest && cs.minFilter == vk::kFilterNearest && cs.maxLod == 0.25f && cs.addressModeU == vk::kAddressClampToEdge,
           "composite sampler: point/point, base map, clamp to edge");

    // kHdr by hand (Xenia xe_gpu_texture_fetch_t, xenos.h:1206-1307): dword 0 type 2, signs (bits 2-9) 0 -> unsigned, no
    // gamma; tiled (bit 31), pitch bits 22-30 = 40 -> 1280. dword 1: format 0x36 = 54, endian (0xB6 >> 6) & 3 = 2 (8in32),
    // base 1BB71000. dword 2: width 0x4FF + 1 = 1280, height 0x2CF + 1 = 720. dword 3 swizzle bits 1-12 = 0x60A: x 2 (Z),
    // y 1 (Y), z 0 (X), w 3 (W) -> B G R A, the resolve's red/blue swap. Staging: 1280 x 720 x 4 = 3,686,400 bytes.
    const PreparedTexture hdr = m.Prepare(kHdr, shader, false, kFull);
    Expect(hdr.entry != nullptr && hdr.plan.stagingBytes == 3686400 && !hdr.binding.gamma &&
               Components(hdr.binding, vk::kSwizzleB, vk::kSwizzleG, vk::kSwizzleR, vk::kSwizzleA),
           "HDR resolve texture: k_2_10_10_10_AS_16_16_16_16 (54) 1280x720 8in32 at 1BB71000, B G R A, no gamma, 3,686,400 bytes");
    // kLum by hand: dword 0 type 2, signs (bits 2-9) 0x55 = 1 1 1 1 (signed), tiled, pitch 1 -> 32. dword 1: format 0x24 =
    // 36 (k_32_FLOAT), endian 2, base 08D4F000. dword 2: 32 x 32. dword 3 swizzle 0xB68: x 0 (X), y 5, z 5, w 5 (ONE) ->
    // R 1 1 1 through RRRR; dword 3 bit 0 (number format) 1, ignored for float. Xenia: R32_SFLOAT for both signednesses (vulkan_texture_cache.cc:275-279), so "signed" changes
    // nothing. Staging: 32 x 32 x 4 = 4,096 bytes.
    const PreparedTexture lum = m.Prepare(kLum, shader, false, kFull);
    Expect(lum.entry != nullptr && lum.plan.stagingBytes == 4096 && !lum.binding.gamma &&
               Components(lum.binding, vk::kSwizzleR, vk::kSwizzleOne, vk::kSwizzleOne, vk::kSwizzleOne),
           "luminance texture: k_32_FLOAT (36) 32x32 8in32 at 08D4F000, signed ignored for float, R 1 1 1, 4,096 bytes");
    {
        // kLut by hand: dword 0 type 2, signs 0 (unsigned), linear (bit 31 clear); dword 1 format 6 (k_8_8_8_8), endian 2,
        // base 11A6E000; 3D 16x16x16 (the stop's own decode). Host upload packed: 16 x 16 x 16 x 4 = 16,384 bytes. Fetched
        // as 3D (dimension 2).
        ShaderFetch s3 = shader;
        s3.dimension = 2;
        const PreparedTexture lut = m.Prepare(kLut, s3, false, kFull);
        Expect(lut.entry != nullptr && lut.plan.stagingBytes == 16384 && !lut.binding.gamma,
               "3D lookup texture: k_8_8_8_8 (6) 16x16x16 linear 8in32 at 11A6E000, 16,384 bytes packed, no gamma");
    }
    const PreparedTexture y = m.Prepare(kY, shader, false, kFull);
    const PreparedTexture u = m.Prepare(kU, shader, false, kFull);
    Expect(y.entry != nullptr && y.plan.stagingBytes == 921600 && u.entry != nullptr && u.plan.stagingBytes == 230400,
           "video planes: Y k_8 1280x720 = 921,600 bytes, U k_8 640x360 = 230,400 bytes");
    Expect(Components(y.binding, vk::kSwizzleR, vk::kSwizzleR, vk::kSwizzleR, vk::kSwizzleOne) && !y.binding.gamma,
           "Y view: X X X 1 through RRRR = R R R ONE, no gamma");
    Expect(y.binding.sampler == u.binding.sampler && y.binding.sampler == us && SamplerDescriptionHash{}(y.binding.sampler) == SamplerDescriptionHash{}(us),
           "sampler key: Y, U and UI share one sampler (equal fields, equal hash)");
    Expect(!(cs == us), "CONTROL: the composite's point sampler is a different key");
    const ViewKey a{ui.entry->key, {3, 4, 5, 6}}, b{ui.entry->key, {5, 4, 3, 6}}, c{comp.entry->key, {3, 4, 5, 6}};
    Expect(a == ViewKey{ui.entry->key, {3, 4, 5, 6}} && !(a == b) && !(a == c), "view key: one image and one mapping; another mapping or image is another view");
}

void TestRefusals()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const ShaderFetch shader{};
    PreparedTexture p = m.Prepare(kUi, shader, true, {false, true});
    Expect(p.entry == nullptr && Has(p.error, "textureCompressionBC") && Has(p.error, "11A8F000"), "CONTROL: DXT1 without textureCompressionBC is refused, named");
    {
        uint32_t intUi[6];
        for (int i = 0; i < 6; i++)
            intUi[i] = kUi[i];
        intUi[3] |= 1;  // dword 3 bit 0: the number-format bit on a normalized (DXT1) texture
        const PreparedTexture q = m.Prepare(intUi, shader, true, kFull);
        Expect(q.entry == nullptr && Has(q.error, "integer number format"), "CONTROL: the number-format bit is still refused on a normalized format");
    }
    p = m.Prepare(kComp, shader, true, {true, false});
    Expect(p.entry == nullptr && Has(p.error, "imageViewFormatSwizzle"), "CONTROL: B G R A without imageViewFormatSwizzle is refused");
    p = m.Prepare(kUi, shader, true, {true, false});
    Expect(p.entry != nullptr, "an identity mapping needs no imageViewFormatSwizzle");
    p = m.Prepare(kUi, shader, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "gamma"), "CONTROL: a gamma decision that disagrees with DrawState's is refused");
    // FM4 Recomp: format 18 (k_DXT1) joined the observed set: a 64x64 linear DXT1 is 16 x 16 blocks of
    // 8 bytes = 2,048 bytes, BC1 as format 51.
    p = m.Prepare(kDxt1Format18, shader, false, kFull);
    Expect(p.entry != nullptr && p.plan.stagingBytes == 2048 && p.entry->description.host.vkFormat == vk::kFormatBc1RgbaUnormBlock,
           "format 18 (k_DXT1) 64x64 is accepted: BC1, 2,048 bytes");
    p = m.Prepare(kRgba8Format6, shader, false, kFull);
    Expect(p.entry != nullptr && p.plan.stagingBytes == 64u * 64u * 4u, "format 6 (k_8_8_8_8) 2D 64x64 is now accepted: 16,384 bytes");
    p = m.Prepare(kDxt1Mips, shader, false, kFull);
    bool halving = p.entry != nullptr && p.entry->description.levelCount == 9;
    if (halving)
    {
        const std::vector<CopyRegion> r = CopyRegions(p.entry->description, p.plan, 0);
        halving = r.size() == 9;
        for (uint32_t k = 0; halving && k < 9; k++)
            halving = r[k].level == k && r[k].width == (256u >> k) && r[k].height == (256u >> k) && r[k].bufferOffset == p.plan.levelOffsets[k];
    }
    Expect(halving, "a 9-level 256x256 DXT1 (packed from level 4) is accepted: one copy region per level, 256 >> k texels (1x1 at level 8)");
    ShaderFetch cube;
    cube.dimension = 3;
    p = m.Prepare(kUi, cube, true, kFull);
    Expect(p.entry == nullptr && Has(p.error, "dimension"), "CONTROL: a cube fetch of a 2D constant is refused");
}

void TestValidity()
{
    const ShaderFetch shader{};
    {
        TextureUnitModel m(TextureValidity::ReloadEveryBind);
        PreparedTexture p = m.Prepare(kY, shader, false, kFull);
        Expect(p.entry != nullptr && p.load && m.Loaded(*p.entry, p.plan), "reload every bind: the first bind loads");
        p = m.Prepare(kY, shader, false, kFull);
        Expect(p.entry != nullptr && p.load, "reload every bind: an unchanged texture loads again (title stores are invisible to this version)");
        Expect(m.Loaded(*p.entry, p.plan) && p.entry->generation == 2, "each load is a new generation (2)");
    }
    TextureUnitModel m(TextureValidity::WriteWatch);
    PreparedTexture p = m.Prepare(kComp, shader, true, kFull);
    Expect(p.entry != nullptr && p.load && m.Loaded(*p.entry, p.plan), "watch: the first bind loads");
    p = m.Prepare(kComp, shader, true, kFull);
    Expect(p.entry != nullptr && !p.load, "watch: nothing written, no load");
    m.MarkWritten(0x13F2A000u, 0x1000);  // the page just below the base: ends exactly where the texture starts
    p = m.Prepare(kComp, shader, true, kFull);
    Expect(p.entry != nullptr && !p.load, "watch: a GPU write that ends at the base does not invalidate");
    m.Retired();
    const uint32_t end = 0x13F2B000u + p.entry->description.base.size;
    m.MarkWritten(end - 1, 1);  // the texture's last byte
    Expect(m.Pending(), "a marked GPU write is pending until retired");
    p = m.Prepare(kComp, shader, true, kFull);
    Expect(p.entry == nullptr && Has(p.error, "not confirmed complete") && Has(p.error, "13F2B000"),
           "CONTROL: a texture over a pending GPU write is refused (never read before the GPU wrote it)");
    m.Retired();
    p = m.Prepare(kComp, shader, true, kFull);
    Expect(!m.Pending() && p.entry != nullptr && p.load, "after the retire the written texture is stale and loads (MarkGuestWritten invalidates)");
    Expect(m.Loaded(*p.entry, p.plan) && p.entry->generation == 2, "the reload is generation 2");
    p = m.Prepare(kComp, shader, true, kFull);
    Expect(p.entry != nullptr && !p.load, "and is valid again");
}

// FM4 Recomp (FM4_RESOLVE_DEFER): the model's pending queries. kUi's base at 11A8F000 (1280x720 DXT1, tiled): a resolve marked over
// it makes the texture read a pending range; one just past its extent does not; retired, nothing is pending.
void TestReadsPending()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    Expect(!m.ReadsPending(kUi), "nothing marked: the UI texture reads no pending range");
    m.MarkWritten(0x11A8F000u + 0x100, 16);
    Expect(m.ReadsPending(kUi) && m.OverlapsPending(0x11A8F000u + 0x10F, 1) && !m.OverlapsPending(0x11A8F000u + 0x110, 1),
           "a resolve marked inside the UI texture: it reads a pending range; the range query's edges");
    m.Retired();
    Expect(!m.ReadsPending(kUi) && !m.OverlapsPending(0x11A8F000u + 0x100, 16), "retired: nothing pending");
}

void TestGuestWriteLog()
{
    GuestWriteLog log;
    GuestRange hit;
    log.Mark(0x13F2B000u, 0);
    Expect(!log.Pending() && log.Marks() == 0, "an empty write marks nothing");
    log.Mark(0x13F2B000u, 3686400);
    Expect(log.Pending() && log.Overlaps(0x13F2B000u + 3686399, 1, hit) && hit.physical == 0x13F2B000u && hit.size == 3686400,
           "overlap: the range's last byte");
    Expect(!log.Overlaps(0x13F2A000u, 0x1000, hit) && !log.Overlaps(0x13F2B000u + 3686400, 1, hit) && !log.Overlaps(0x13F2B000u, 0, hit),
           "CONTROL: adjacent ranges and an empty range do not overlap");
    log.Retired();
    Expect(!log.Pending() && !log.Overlaps(0x13F2B000u, 1, hit) && log.Marks() == 1, "retired: nothing pending, the mark counted");
}

void TestStagingBytes()
{
    // Tiled DXT1 16x8 (4 x 2 blocks of 8 bytes, log2 3), pitch 32 blocks. GetTiledOffset2D (texture_util.cc:443-458)
    // by hand, macro = 0 (x, y < 32):
    //   micro = ((x & 7) + ((y & 0xE) << 2)) << 3; offset = ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    //   result = offset for offset < 0x40 (the other terms are 0 for x < 8, y < 8).
    //   (0,0): 0. (1,0): micro 8, offset 8. (2,0): micro 16, (16 << 1) = 32. (3,0): micro 24, 32 + 8 = 40.
    //   (0,1): micro 0, + 16 = 16. (1,1): 8 + 16 = 24. (2,1): 32 + 16 = 48. (3,1): 40 + 16 = 56.
    // Output rows: (0,0) (1,0) (2,0) (3,0) (0,1) (1,1) (2,1) (3,1). 8in16 swaps the bytes of each 16-bit half
    // ({1,0,3,2} per dword): output byte j of a block is source byte j ^ 1. The source extent is
    // GetTiledAddressUpperBound2D (:490-517): the tile at (0, 0) + 0x400 << 3 = 8192 bytes.
    const uint32_t offsets[8] = {0, 8, 32, 40, 16, 24, 48, 56};
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kDxt1Small, ShaderFetch{}, false, kFull);
    Expect(p.entry != nullptr && p.plan.stagingBytes == 64 && p.entry->description.levels[0].extentBytes == 8192,
           "small DXT1: 64 staging bytes, an 8192-byte tiled source extent");
    if (p.entry == nullptr)
        return;
    FakeGuest g = Pattern(kSmallBase, 8192);
    std::vector<uint8_t> out(p.plan.stagingBytes, 0xEE);
    std::string error;
    Expect(FillStaging(p.entry->description, p.plan, GuestOf, &g, out.data(), error), "small DXT1: staging filled");
    const uint8_t first[16] = {1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14};  // blocks (0,0) at 0 and (1,0) at 8, swapped
    bool all = std::memcmp(out.data(), first, 16) == 0;
    for (uint32_t k = 0; k < 8; k++)
        for (uint32_t j = 0; j < 8; j++)
            all = all && out[k * 8 + j] == uint8_t(offsets[k] + (j ^ 1));
    Expect(all, "small DXT1: every block from its hand-computed tiled offset, 8in16-swapped");
    Expect(out[16] == 33 && out[32] == 17, "small DXT1: block (2,0) comes from 32 and (0,1) from 16 (tiled, not row-major)");
    const PreparedTexture q = m.Prepare(kDxt1SmallNoSwap, ShaderFetch{}, false, kFull);
    std::vector<uint8_t> raw(64);
    Expect(q.entry != nullptr && FillStaging(q.entry->description, q.plan, GuestOf, &g, raw.data(), error) && raw[0] == 0 && raw[1] == 1 && raw[0] != out[0],
           "CONTROL: endian 0 leaves the bytes unswapped (the swap is the constant's, not a default)");
    g.refuse = true;
    Expect(!FillStaging(p.entry->description, p.plan, GuestOf, &g, out.data(), error) && Has(error, "not in one view"),
           "CONTROL: a guest range outside one view is refused, named");

    // Linear k_8 5x2, row pitch 32 bytes (pitch 32 texels; TiledPitchBlocks(32, 1) = 32): rows at 0 and 32, so
    // the staging is source bytes 0-4 then 32-36; extent 32 x (2 - 1) + 5 = 37 (texture_util.cc:420-423).
    const PreparedTexture k = m.Prepare(kK8Small, ShaderFetch{}, false, kFull);
    Expect(k.entry != nullptr && k.plan.stagingBytes == 16 && k.plan.levelBytes[0] == 10 && k.entry->description.levels[0].extentBytes == 37,
           "small k_8: 10 bytes (16 with the 16-byte rounding), a 37-byte linear extent");
    if (k.entry == nullptr)
        return;
    FakeGuest small = Pattern(kSmallBase, 37);
    std::vector<uint8_t> plane(16, 0xEE);
    const uint8_t expected[10] = {0, 1, 2, 3, 4, 32, 33, 34, 35, 36};
    Expect(FillStaging(k.entry->description, k.plan, GuestOf, &small, plane.data(), error) && std::memcmp(plane.data(), expected, 10) == 0,
           "small k_8: rows 0-4 and 32-36 (the pitch, not the width, separates rows)");
    FakeGuest shorter = Pattern(kSmallBase, 36);
    Expect(!FillStaging(k.entry->description, k.plan, GuestOf, &shorter, plane.data(), error), "CONTROL: a guest view one byte short of the extent is refused");

    const std::vector<CopyRegion> regions = CopyRegions(k.entry->description, k.plan, 4096);
    Expect(regions.size() == 1 && regions[0].bufferOffset == 4096 && regions[0].level == 0 && regions[0].width == 5 && regions[0].height == 2,
           "copy region: the ring offset, level 0, 5x2 texels");
}

// 3D textures (draw 33808's slot 7, the local capture draw_pred5 (not distributed)). Hand-computed from Xenia 67f16c4e315f texture_util.cc:461-487
// GetTiledOffset3D with pitch 32, height 32, 4-byte blocks (log2 2): pitch >> 5 = 1, height >> 4 = 2;
//   macro_outer = (y >> 4) + 2 (z >> 2); macro = ((x >> 5) + macro_outer) << 9; micro = ((x & 7) + ((y & 6) << 2)) << 2;
//   offset_outer = ((y >> 3) + (z >> 2)) & 1; offset1 = offset_outer + ((((x >> 3) + 2 offset_outer) & 3) << 1);
//   offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) + ((z & 3) << 8) + ((y & 1) << 4);
//   address = ((((((offset1 & 1) << 3) + ((offset2 >> 6) & 7)) << 3) + (offset1 & ~1)) << 2) + (offset2 & ~511), << 3, + (offset2 & 63).
//   (1,0,0): micro 4, offset2 4 -> 4.            (0,1,0): offset2 16 -> 16.
//   (0,2,0): micro 32 -> offset2 64 -> (64 >> 6) & 7 = 1 -> 1 << 3 << 2 << 3 = 256.     (1,2,0): micro 36, offset2 68 -> 256 + 4 = 260.
//   (8,0,0): offset1 = (1 & 3) << 1 = 2 -> (2 << 2) << 3 = 64.                        (0,0,1): offset2 = 1 << 8 = 256 -> 4 << 8 = 1024.
//   (0,8,0): offset_outer 1, offset1 = 1 + 4 = 5 -> ((8 << 3) + 4) << 2 << 3 = 2176.  (0,16,0): macro 512, offset2 1024 -> 1024 << 3 = 8192.
//   (0,0,4): macro_outer 2, macro 1024, offset2 2048, offset1 5 -> ((64 + 4) << 2 + 2048) << 3 = 18560 (slices 4-7 stored apart).
// GetTiledAddressUpperBound3D (:520-551): the tile origin plus ((pitch << 6) + 0x800) << log2 (4096 << 2 = 16384 for pitch 32,
// log2 2), or for 1-byte blocks (pitch / 64) * 0x1000 + 0xC00 + (pitch & 32) * 32 (pitch 32: 0x1000; pitch 64: 0x1C00).
// Every value was also printed by an independent Python transcription of the same lines.
void Test3D()
{
    struct Row { uint32_t x, y, z, expected; };
    const Row rows[] = {{0, 0, 0, 0},     {1, 0, 0, 4},       {0, 1, 0, 16},     {0, 2, 0, 256},   {1, 2, 0, 260},   {8, 0, 0, 64},
                        {0, 0, 1, 1024}, {1, 0, 1, 1028},   {0, 1, 1, 1040},   {0, 8, 0, 2176},  {0, 16, 0, 8192}, {0, 0, 4, 18560}};
    bool all = true;
    for (const Row& r : rows)
        if (TiledOffset3D(r.x, r.y, r.z, 32, 32, 2) != r.expected)
        {
            all = false;
            std::printf("  TiledOffset3D(%u, %u, %u) = %u, expected %u\n", r.x, r.y, r.z, TiledOffset3D(r.x, r.y, r.z, 32, 32, 2), r.expected);
        }
    Expect(all, "TiledOffset3D: 12 hand-computed offsets (pitch 32, height 32, 4-byte blocks)");
    Expect(TiledOffset3D(0, 0, 1, 32, 3, 2) == 1024 && TiledOffset3D(0, 0, 4, 20, 3, 2) == 18560, "TiledOffset3D aligns pitch and height to 32 (:464-465)");
    Expect(TiledAddressUpperBound3D(2, 3, 2, 32, 3, 2) == 16384 && TiledAddressUpperBound3D(32, 32, 8, 32, 32, 2) == 18560 + 16384 &&
               TiledAddressUpperBound3D(1, 1, 1, 32, 1, 0) == 0x1000 && TiledAddressUpperBound3D(1, 1, 1, 64, 1, 0) == 0x1C00 &&
               TiledAddressUpperBound3D(0, 1, 1, 32, 1, 2) == 0,
           "TiledAddressUpperBound3D: 16384, 34944 (slices 4-7), 0x1000 / 0x1C00 for 1-byte blocks, 0 when empty");

    // A tiled k_8_8_8_8_AS_16_16_16_16 (50) 2x3x2: dword 0 type 2, clamp x y z 2, pitch 1 << 22 (32 texels), tiled; dword 1 format
    // 50 at 0x00100000, endian 0; dword 2 size_3d 1 | 2 << 11 | 1 << 22; dword 5 dimension 2 (0x400).
    const uint32_t kTiled3D[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 50u | kSmallBase, 1u | 2u << 11 | 1u << 22,
                                  kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 3, 2u << 9};
    const ShaderFetch volume{3, 3, 3, 7, 2};
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    PreparedTexture p = m.Prepare(kTiled3D, volume, false, kFull);
    Expect(p.entry != nullptr, "tiled 3D 2x3x2 k_8_8_8_8 is accepted for a 3D fetch");
    if (p.entry == nullptr)
    {
        std::printf("  %s\n", p.error.c_str());
        return;
    }
    const TextureDescription& d = p.entry->description;
    Expect(d.dimension == 2 && d.width == 2 && d.height == 3 && d.depth == 2 && d.levels[0].depth == 2 && d.levels[0].zStrideBlockRows == 32 &&
               d.levels[0].pitchBlocks == 32 && d.levels[0].extentBytes == 16384 && p.plan.stagingBytes == 48,
           "tiled 3D description: 2x3x2, slice stride 32 rows, extent 16384 (the 32x32x4 tile), 48 staging bytes");
    Expect(p.entry->key.dimension == 2 && p.entry->key.depth == 2 && p.entry->key.width == 2 && p.entry->key.height == 3,
           "the cache key carries the 3D size (size_3d fields) and the dimension");
    FakeGuest g{kSmallBase, std::vector<uint8_t>(16384)};
    for (uint32_t o = 0; o < 16384; o += 4)
        std::memcpy(g.bytes.data() + o, &o, 4);  // each 4-byte block holds its own offset
    std::vector<uint8_t> out(p.plan.stagingBytes);
    std::string error;
    const uint32_t expected[12] = {0, 4, 16, 20, 256, 260, 1024, 1028, 1040, 1044, 1280, 1284};  // (x, y, z) in x, y, z order
    Expect(FillStaging(d, p.plan, GuestOf, &g, out.data(), error) && std::memcmp(out.data(), expected, 48) == 0,
           "tiled 3D staging: blocks from 0 4 16 20 256 260 | 1024 1028 1040 1044 1280 1284 (slice 1 at the z & 3 << 8 term)");
    FakeGuest shortTile{kSmallBase, std::vector<uint8_t>(16380)};
    Expect(!FillStaging(d, p.plan, GuestOf, &shortTile, out.data(), error), "CONTROL: a guest view short of the 3D tile extent is refused");
    const std::vector<CopyRegion> regions = CopyRegions(d, p.plan, 512);
    Expect(regions.size() == 1 && regions[0].width == 2 && regions[0].height == 3 && regions[0].depth == 2 && regions[0].bufferOffset == 512,
           "copy region: 2x3 texels, depth 2 (imageExtent.depth)");
    Expect(p.binding.sampler.addressModeW == vk::kAddressClampToEdge, "3D: W clamp is the constant's z clamp (2, clamp to edge)");
    const uint32_t kTiled3DRepeatZ[6] = {kTiled3D[0] & ~(7u << 16), kTiled3D[1], kTiled3D[2], kTiled3D[3], kTiled3D[4], kTiled3D[5]};
    p = m.Prepare(kTiled3DRepeatZ, volume, false, kFull);
    Expect(p.entry != nullptr && p.binding.sampler.addressModeW == vk::kAddressRepeat && p.binding.sampler.addressModeU == vk::kAddressClampToEdge,
           "3D: z clamp 0 gives W repeat (texture_util.cc GetClampModesForDimension)");
    p = m.Prepare(kUi, ShaderFetch{}, true, kFull);
    Expect(p.entry != nullptr && p.binding.sampler.addressModeW == vk::kAddressClampToEdge, "2D: W stays clamp to edge whatever the z field");

    // A linear k_8 3x2x2, pitch 32 texels: rows 32 bytes apart, slices 32 rows (1024 bytes) apart (texture_address.xesli:164-166);
    // extent 1024 + 32 + 3 = 1059 (texture_util.cc:420-423). Source byte i = i % 251: slice 0 rows 0-2 and 32-34, slice 1 rows
    // 1024-1026 (20-22) and 1056-1058 (52-54).
    const uint32_t kLinear3D[6] = {2u | kClampEdgeXYZ | 1u << 22, 2u | kSmallBase, 2u | 1u << 11 | 1u << 22, kLinearMagMin | kMipBaseMap, 3, 2u << 9};
    p = m.Prepare(kLinear3D, volume, false, kFull);
    Expect(p.entry != nullptr && p.entry->description.levels[0].extentBytes == 1059 && p.plan.stagingBytes == 16 && p.plan.levelBytes[0] == 12,
           "linear 3D k_8 3x2x2: extent 1059, 12 bytes");
    if (p.entry != nullptr)
    {
        FakeGuest lin{kSmallBase, std::vector<uint8_t>(1059)};
        for (size_t i = 0; i < lin.bytes.size(); i++)
            lin.bytes[i] = uint8_t(i % 251);
        std::vector<uint8_t> plane(16, 0xEE);
        const uint8_t want[12] = {0, 1, 2, 32, 33, 34, 20, 21, 22, 52, 53, 54};
        Expect(FillStaging(p.entry->description, p.plan, GuestOf, &lin, plane.data(), error) && std::memcmp(plane.data(), want, 12) == 0,
               "linear 3D staging: slice 1 starts 32 rows (1024 bytes) after slice 0");
    }

    // Refusals.
    p = m.Prepare(kTiled3D, ShaderFetch{}, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "dimension 1 but the constant's dimension is 2"), "CONTROL: a 2D fetch of a 3D constant is refused");
    const uint32_t kMips3D[6] = {kTiled3D[0], kTiled3D[1], 63u | 63u << 11 | 3u << 22, kTiled3D[3], 3u | 2u << 6, 2u << 9 | 0x00200000u};
    p = m.Prepare(kMips3D, volume, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "3D texture with mip levels"), "CONTROL: a 3D texture with mips is refused");
    const uint32_t kPacked3D[6] = {kTiled3D[0], kTiled3D[1], kTiled3D[2], kTiled3D[3], kTiled3D[4], 2u << 9 | 1u << 11};
    p = m.Prepare(kPacked3D, volume, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "packed mip tail"), "CONTROL: a 3D texture whose base is a packed mip tail is refused");
    const uint32_t kStacked3D[6] = {kTiled3D[0], kTiled3D[1] | 1u << 10, kTiled3D[2], kTiled3D[3], kTiled3D[4], kTiled3D[5]};
    p = m.Prepare(kStacked3D, volume, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "stacked"), "CONTROL: a stacked constant (an array) is refused");
    const uint32_t kCube[6] = {kUi[0], kUi[1], kUi[2], kUi[3], kUi[4], 3u << 9};
    ShaderFetch cube;
    cube.dimension = 3;
    p = m.Prepare(kCube, cube, true, kFull);
    Expect(p.entry == nullptr && Has(p.error, "stack depth is 0"), "CONTROL: a cube constant with one face (stack depth 0) is refused");
}

// Mipmaps: FM4's draw 61145 slot 0 (the local capture window4 (not distributed)), decoded by hand (Xenia 67f16c4e315f xenos.h:1206-1307):
//   dword 0 80420002: type 2; signs 0; clamp x 0, y 0 (repeat), z (0x8042 & 7 = 2, not used for 2D); pitch 0x201 & 0x1FF = 1 -> 32
//            texels; tiled (bit 31).
//   dword 1 11A68086: format 0x86 & 0x3F = 6 (k_8_8_8_8), endian (0x86 >> 6) & 3 = 2 (8in32), not stacked, base 11A68000.
//   dword 2 0003E01F: width 0x1F + 1 = 32, height (0x3E01F >> 13) + 1 = 32.
//   dword 3 00281414: number format 0; swizzle (0x1414 >> 1) & 0xFFF = 0xA0A = Z Y X 1; exp adjust 0; mag bit 19 = 1 (linear),
//            min bits 21-22 = 1 (linear), mip bits 23-24 = 0 (kPoint, xenos.h:131-138 TextureFilter: 0 point, 1 linear,
//            2 base map, 3 use fetch constant); aniso 0.
//   dword 4 00000043: vol mag/min 1 1; mip_min_level (0x43 >> 2) & 0xF = 0, mip_max_level (0x43 >> 6) & 0xF = 1; LOD bias 0.
//   dword 5 11A69A00: border 0; dimension (0xA00 >> 9) & 3 = 1 (2D); packed mips (bit 11) = 1; mip address 11A69000.
// Levels (texture_util.cc:60-92): mip page != 0, size_mip_max_level = log2_floor(32) = 5, so levels 0-1. Packed level
// (texture_util.h:36-39): log2_ceil(min(32, 32)) = 5 > 4 -> 5 - 4 = 1: level 1 is the packed tail, stored at the mip address
// (offset 0: no unpacked mips before it, :429-436). Its position in the tail (:146-165, packed_mip = 1 - 1 = 0, square takes the
// "taller than wide" branch): x = 16 >> 0 = 16 blocks, y = 0. Its row pitch (:310-322): next_pow2(32) >> 1 = 16 texels -> 32
// blocks (tile-aligned) * 4 = 128 bytes. Its extent (:351-401, :405-418): x extent 16 + 16 = 32, y extent 0 + 16 = 16 ->
// GetTiledAddressUpperBound2D(32, 16, 32, 2) = 0 + 0x400 << 2 = 4096. The base: pitch 32 blocks, extent (32, 32) -> 4096, so the
// base ends exactly at the mip address. Level-1 texel (x, y) is tiled block (16 + x, y) from 11A69000 (GetTiledOffset2D :443-458):
//   (0,0): x >> 3 = 2 -> ((0 + 2) & 3) << 6 = 128.  (1,0): + micro 4 = 132.  (0,1): + (y & 1) << 4 = 144.  (8,0): (3 & 3) << 6 = 192.
//   (0,8): micro ((8 & 0xE) << 2) << 2 = 128 -> offset 256 -> (256 & 0x1C0) << 2 = 1024, ((2 + 2) & 3) << 6 = 0 -> 1024.
// An independent transcription prints the same values, and (15,15) -> 1916, level 0 (31,31) -> 3964.
// Sampler (vulkan_texture_cache.cc:611-690, :755-820): mip point -> VK_SAMPLER_MIPMAP_MODE_NEAREST, minLod = mip_min_level 0,
// maxLod VK_LOD_CLAMP_NONE (not base map); U/V repeat; mag/min linear (texture_info.h's documented difference: Xenia's Vulkan
// backend takes min and mip from mag).
void TestMips()
{
    const uint32_t kMipped[6] = {(1u << 31 | 0x00420002u), 0x11A68086u, 0x0003E01Fu, 0x00281414u, 0x00000043u, 0x11A69A00u};
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kMipped, ShaderFetch{}, false, kFull);
    Expect(p.entry != nullptr, "draw 61145 slot 0: 32x32 k_8_8_8_8 with levels 0-1 is accepted");
    if (p.entry == nullptr)
    {
        std::printf("  %s\n", p.error.c_str());
        return;
    }
    const TextureDescription& d = p.entry->description;
    Expect(d.levelCount == 2 && d.packedLevel == 1 && d.fetch.endian == 2 && d.levels[0].address == 0x11A68000u && d.levels[0].pitchBlocks == 32 &&
               d.levels[0].extentBytes == 4096,
           "levels 0-1, packed level 1, 8in32; level 0 at 11A68000, pitch 32 blocks, extent 4096");
    const MipLevel& l1 = d.levels[1];
    Expect(l1.address == 0x11A69000u && l1.packed && l1.x == 16 && l1.y == 0 && l1.widthBlocks == 16 && l1.heightBlocks == 16 && l1.pitchBlocks == 32 &&
               l1.rowPitchBytes == 128 && l1.extentBytes == 4096,
           "level 1: the packed tail at the mip address 11A69000, at block (16, 0), 16x16, pitch 32 blocks (128 bytes), extent 4096");
    Expect(d.base.physical == 0x11A68000u && d.base.size == 4096 && d.mips.physical == 0x11A69000u && d.mips.size == 4096,
           "watch ranges: base 11A68000 + 4096, mips 11A69000 + 4096");
    Expect(p.plan.levelOffsets.size() == 2 && p.plan.levelOffsets[0] == 0 && p.plan.levelBytes[0] == 4096 && p.plan.levelOffsets[1] == 4096 &&
               p.plan.levelBytes[1] == 1024 && p.plan.stagingBytes == 5120,
           "staging: level 0 4096 bytes at 0, level 1 16 x 16 x 4 = 1024 bytes at 4096");
    const SamplerDescription& s = p.binding.sampler;
    Expect(s.mipmapMode == vk::kMipmapModeNearest && s.minLod == 0.0f && s.maxLod == vk::kLodClampNone && s.magFilter == vk::kFilterLinear &&
               s.minFilter == vk::kFilterLinear && s.addressModeU == vk::kAddressRepeat && s.addressModeV == vk::kAddressRepeat &&
               s.addressModeW == vk::kAddressClampToEdge,
           "sampler: mip nearest (point), minLod 0, maxLod none, linear/linear, repeat");
    Expect(Components(p.binding, vk::kSwizzleB, vk::kSwizzleG, vk::kSwizzleR, vk::kSwizzleOne) && !p.binding.gamma, "view: Z Y X 1 = B G R ONE");
    const std::vector<CopyRegion> r = CopyRegions(d, p.plan, 256);
    Expect(r.size() == 2 && r[0].bufferOffset == 256 && r[0].width == 32 && r[0].height == 32 && r[1].level == 1 && r[1].bufferOffset == 256 + 4096 &&
               r[1].width == 16 && r[1].height == 16,
           "copy regions: level 0 32x32 at the ring offset, level 1 16x16 4096 bytes later");
    // Guest memory 11A68000-11A69FFF; each dword holds its byte offset from 11A68000 (little-endian); 8in32 reverses the bytes.
    FakeGuest g{0x11A68000u, std::vector<uint8_t>(8192)};
    for (uint32_t o = 0; o < 8192; o += 4)
        std::memcpy(g.bytes.data() + o, &o, 4);
    std::vector<uint8_t> out(p.plan.stagingBytes);
    std::string error;
    const bool filled = FillStaging(d, p.plan, GuestOf, &g, out.data(), error);
    auto word = [&](size_t byte) {
        uint32_t v = 0;
        std::memcpy(&v, out.data() + byte, 4);
        return v;
    };
    Expect(filled && word(0) == __builtin_bswap32(0u) && word(4) == __builtin_bswap32(4u), "level 0: texels (0,0), (1,0) from base + 0, + 4, 8in32");
    const size_t l1base = 4096;
    Expect(filled && word(l1base) == __builtin_bswap32(4096u + 128) && word(l1base + 4) == __builtin_bswap32(4096u + 132) &&
               word(l1base + 16 * 4) == __builtin_bswap32(4096u + 144) && word(l1base + 8 * 4) == __builtin_bswap32(4096u + 192) &&
               word(l1base + 8 * 16 * 4) == __builtin_bswap32(4096u + 1024) && word(l1base + (15 * 16 + 15) * 4) == __builtin_bswap32(4096u + 1916),
           "level 1: texels (0,0) (1,0) (0,1) (8,0) (0,8) (15,15) from mip address + 128 132 144 192 1024 1916 (the tail's block (16 + x, y))");
    FakeGuest shortMips{0x11A68000u, std::vector<uint8_t>(8188)};
    Expect(!FillStaging(d, p.plan, GuestOf, &shortMips, out.data(), error), "CONTROL: a guest view one dword short of the mip tail's extent is refused");
    const uint32_t kNoPacked[6] = {kMipped[0], kMipped[1], kMipped[2], kMipped[3], kMipped[4], kMipped[5] & ~(1u << 11)};
    const PreparedTexture q = m.Prepare(kNoPacked, ShaderFetch{}, false, kFull);
    Expect(q.entry != nullptr && !q.entry->description.levels[1].packed && q.entry->description.levels[1].x == 0 &&
               q.entry->description.levels[1].address == 0x11A69000u,
           "CONTROL: without the packed bit level 1 is an ordinary mip at the mip address, block (0, 0) (the bit decides the layout)");
    const uint32_t kBaseMap[6] = {kMipped[0], kMipped[1], kMipped[2], kMipped[3] | 2u << 23, kMipped[4], kMipped[5]};
    const PreparedTexture b = m.Prepare(kBaseMap, ShaderFetch{}, false, kFull);
    Expect(b.entry != nullptr && b.binding.sampler.maxLod == 0.25f && b.binding.sampler.minLod == 0.0f,
           "base-map mip filter: maxLod = minLod + 0.25 (vulkan_texture_cache.cc:804-807), the same two-level image");
}

// The welcome screen's logo glow (ps_93879bb4564bce04, draws 55090-55134; the local capture window3 (not distributed), the `tex 0` line of
// the glow's draws; 864200FC is the same constant with type 0 = invalid in draws that do not sample it). Hand decode (Xenia
// 67f16c4e315f xenos.h:1206-1307):
//   dword 0 864200FE: type 2; signs (0x0FE >> 2) = 0x3F -> x y z 3 (gamma), w 0 (unsigned); clamp x (bits 10-12) 0 and y (13-15)
//            0 = repeat, z (0x8642 & 7 = 2, unused for 2D); pitch (dword 0 >> 22) & 0x1FF = 0x19 = 25 -> 800 texels; tiled.
//   dword 1 117D80B2: format 0x32 = 50 (k_8_8_8_8_AS_16_16_16_16), endian (0xB2 >> 6) & 3 = 2 (8in32), base 117D8000.
//   dword 2 003E631F: width 0x31F + 1 = 800, height (0x3E631F >> 13) + 1 = 0x1F3 + 1 = 500.
//   dword 3 00280C14: number format 0; swizzle (0xC14 >> 1) & 0xFFF = 0x60A = Z Y X W; exp adjust 0; mag 1, min 1 (linear),
//            mip 0 (point); aniso 0.
//   dword 4 00000003: vol filters 1 1; mip_min 0, mip_max 0; LOD bias 0.
//   dword 5 00000A00: border 0, dimension 1 (2D), packed mips 1, mip address 0 -> one level (texture_util.cc:73-76); the packed
//            level (texture_util.h:36-39) is log2_ceil(500) - 4 = 5, so the base is not packed.
// Where the shader's w comes from (the alpha the test sees): Xenia's 32bpb load (texture_load_32bpb.xesli:41-43) applies
// XeEndianSwap32 with endian 8in32 (endian.xesli:37-46: the four bytes reversed) and stores to R8G8B8A8 (k_8_8_8_8 ->
// R8G8B8A8_UNORM, RGBA, vulkan_texture_cache.cc:121-125, :335-339), whose A is bits 24-31 of the swapped word = the guest's
// FIRST byte in memory; the fetch's 4th swizzle component is W (3) -> the view's A. So: alpha = guest byte 0 of each texel.
// Tiled texel offsets (GetTiledOffset2D :443-458, 4-byte blocks, pitch 800): (0,0) 0, (1,0) 4, (0,1) (y & 1) << 4 = 16,
// (8,0) ((x >> 3) & 3) << 6 = 64.
// FM4 Recomp: k_24_8_FLOAT (23), an FM4 depth texture (slot 1). Hand decode (xenos.h:1206-1307):
// 8A024802: type 2, signs 0 (bits 2-9 of 0x4802), clamp 2 2 2, pitch ((1u << 31 | 0x0A024802) >> 22) & 0x1FF = 0x28 -> 1280, tiled.
// 1C421097: format 0x17 = 23, endian (0x97 >> 6) & 3 = 2 (8in32), base 1C421000. 0059E4FF: 1280 x 720. 01000D11: number format
// bit 0 = 1 (float data: ignored, as Xenia never reads it), swizzle (0xD11 >> 1) & 0xFFF = 0x688 = X Y Z W, point mag / min,
// mip base map. Host (Xenia vulkan_texture_cache.cc:210-213): R32_SFLOAT, RRRR -> R R R R. Staging 1280 x 720 x 4 = 3,686,400.
const uint32_t kDepthTex[6] = {1u << 31 | 0x0A024802u, 0x1C421097u, 0x0059E4FFu, 0x01000D11u, 0x00000000u, 0x00000200u};
// A linear k_24_8_FLOAT 5x1, pitch 32 texels, endian 8in32, at 0x00100000, point sampled.
const uint32_t kDepthSmall[6] = {2u | kClampEdgeXYZ | 1u << 22, 23u | 2u << 6 | kSmallBase, 4u, kSwizzleXYZW | kMipBaseMap | 1u, 0, kDim2D};

void TestDepthFloat()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kDepthTex, ShaderFetch{}, false, kFull);
    Expect(p.entry != nullptr && p.plan.stagingBytes == 3686400 && !p.binding.gamma &&
               Components(p.binding, vk::kSwizzleR, vk::kSwizzleR, vk::kSwizzleR, vk::kSwizzleR) &&
               p.entry->description.host.vkFormat == vk::kFormatR32Sfloat,
           "scenario6 depth texture: k_24_8_FLOAT (23) 1280x720 tiled 8in32 at 1C421000, R32_SFLOAT R R R R, 3,686,400 bytes");
    // 20e4 by hand (Xenia pixel_formats.xesli:622-638: exponent bits 20-23, mantissa 0-19, float exponent e + 112, a
    // denormal normalized, 0 -> 0); the guest words big-endian, swapped by 8in32:
    //   F00000AB: d24 F00000, e 15 m 0 -> 2^0 = 1.0 = 3F800000 (stencil AB dropped)
    //   E8000000: e 14, m 0x80000 -> 1.5 * 2^-1 = 0.75 = 3F400000
    //   08000012: e 0, m 0x80000 (denormal) -> 0.5 * 2^(1-15) = 2^-15 = 38000000
    //   00000077: d24 0 -> 0.0
    //   FFFFFFFF: e 15, m 0xFFFFF -> (127 << 23) | (0xFFFFF << 3) = 3FFFFFF8 (just under 2)
    const uint32_t words[5] = {0xF00000ABu, 0xE8000000u, 0x08000012u, 0x00000077u, 0xFFFFFFFFu};
    const uint32_t want[5] = {0x3F800000u, 0x3F400000u, 0x38000000u, 0x00000000u, 0x3FFFFFF8u};
    FakeGuest g{kSmallBase, std::vector<uint8_t>(128, 0)};
    for (int i = 0; i < 5; i++)
        for (int b = 0; b < 4; b++)
            g.bytes[size_t(4 * i + b)] = uint8_t(words[i] >> (24 - 8 * b));
    const PreparedTexture s = m.Prepare(kDepthSmall, ShaderFetch{}, false, kFull);
    std::vector<uint8_t> out(s.entry != nullptr ? s.plan.stagingBytes : 0, 0xEE);
    std::string error;
    bool ok = s.entry != nullptr && FillStaging(s.entry->description, s.plan, GuestOf, &g, out.data(), error);
    for (int i = 0; ok && i < 5; i++)
    {
        uint32_t u;
        std::memcpy(&u, out.data() + 4 * i, 4);
        ok = u == want[i];
    }
    Expect(ok, "k_24_8_FLOAT staging: each word becomes the float32 of its 20e4 depth (1.0, 0.75, 2^-15, 0, 0x3FFFFFF8)");
    uint32_t asFloat32[6];
    std::memcpy(asFloat32, kDepthSmall, sizeof asFloat32);
    asFloat32[1] = 36u | 2u << 6 | kSmallBase;  // k_32_FLOAT: the same words uploaded unchanged
    const PreparedTexture f = m.Prepare(asFloat32, ShaderFetch{}, false, kFull);
    uint32_t first = 0;
    Expect(f.entry != nullptr && FillStaging(f.entry->description, f.plan, GuestOf, &g, out.data(), error) &&
               (std::memcpy(&first, out.data(), 4), first == 0xF00000ABu),
           "CONTROL: k_32_FLOAT leaves the word as it is (the 20e4 transform is k_24_8_FLOAT's)");
    uint32_t signedDepth[6];
    std::memcpy(signedDepth, kDepthTex, sizeof signedDepth);
    signedDepth[0] |= 1u << 2;  // sign of component X: signed (Xenia has no signed k_24_8_FLOAT host format)
    const PreparedTexture q = m.Prepare(signedDepth, ShaderFetch{}, false, kFull);
    Expect(q.entry == nullptr && Has(q.error, "signed component 0"), "CONTROL: a signed k_24_8_FLOAT component is refused");
}

// FM4 Recomp: k_DXT4_5_AS_16_16_16_16 (53), an FM4 texture (slot 0). Hand decode (xenos.h:1206-1307):
// 820248FE: type 2, signs (0x48FE >> 2) & 0xFF = 0x3F -> 3 3 3 0 (x y z gamma), clamp 2 2 2, pitch ((1u << 31 | 0x020248FE) >> 22) &
// 0x1FF = 8 -> 256, tiled. 11946075: format 0x35 = 53, endian (0x75 >> 6) & 3 = 1 (8in16), base 11946000. 000FE0FF: 256 x 128.
// 00280D10: swizzle 0x688 = X Y Z W, mag 1, min 1, mip 0. 00000A00: 2D, one level. Host (Xenia vulkan_texture_cache.cc:350-354):
// BC3_UNORM (137), RGBA, 16-byte 4x4 blocks: 64 x 32 blocks x 16 = 32,768 bytes; gamma as the UI's DXT1.
const uint32_t kDxt5[6] = {1u << 31 | 0x020248FEu, 0x11946075u, 0x000FE0FFu, 0x00280D10u, 0x00000003u, 0x00000A00u};
// A tiled DXT5 8x8 texels (2x2 blocks of 16 bytes), 8in16, pitch 32 texels, at 0x00100000.
const uint32_t kDxt5Small[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 53u | 1u << 6 | kSmallBase, 7u | 7u << 13,
                                kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};

// FM4 Recomp: k_DXT4_5 (20), the local capture dynloop1 (not distributed) draw 460886 slot 0. Hand decode: 840200FE: type 2, signs 3 3 3 0 (gamma), clamp
// 2 2 2 (0x00FE bits 10-15 = 0; dword 0 bits 16-18 = 2; x y: (0xFE >> 10) = 0 -> repeat), pitch ((1u << 31 | 0x040200FE) >> 22) & 0x1FF = 0x10 ->
// 512, tiled. 0A98E054: format 0x14 = 20, endian 1 (8in16), base 0A98E000. 003FE1FF: 512 x 512. 00000243: mip min 0, max
// (0x243 >> 6) & 0xF = 9. 0A971A00: 2D, mip address 0A971000. Ten levels 512 .. 1 of 16-byte blocks: 128^2, 64^2, 32^2, 16^2,
// 8^2, 4^2, 2^2 blocks then three 1-block levels: 262,144 + 65,536 + 16,384 + 4,096 + 1,024 + 256 + 64 + 3 x 16 = 349,552 bytes.
const uint32_t kDxt5Mips[6] = {1u << 31 | 0x040200FEu, 0x0A98E054u, 0x003FE1FFu, 0x01280D10u, 0x00000243u, 0x0A971A00u};

void TestDxt5()
{
    {
        TextureUnitModel m(TextureValidity::ReloadEveryBind);
        const PreparedTexture p = m.Prepare(kDxt5Mips, ShaderFetch{}, true, kFull);
        Expect(p.entry != nullptr && p.entry->description.levelCount == 10 && p.plan.stagingBytes == 349552 &&
                   p.entry->description.host.vkFormat == 137 && p.binding.gamma,
               "dynloop1 DXT5: k_DXT4_5 (20) 512x512, 10 levels, BC3, gamma, 349,552 staging bytes");
    }
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kDxt5, ShaderFetch{}, true, kFull);
    Expect(p.entry != nullptr && p.plan.stagingBytes == 32768 && p.binding.gamma && p.entry->description.host.vkFormat == 137 &&
               p.entry->description.host.blockCompressed && Components(p.binding, vk::kSwizzleR, vk::kSwizzleG, vk::kSwizzleB, vk::kSwizzleA),
           "scenario9 DXT5: k_DXT4_5_AS_16_16_16_16 (53) 256x128 tiled 8in16 at 11946000, BC3 R G B A, gamma, 32,768 bytes");
    const PreparedTexture noBc = m.Prepare(kDxt5, ShaderFetch{}, true, {false, true});
    Expect(noBc.entry == nullptr && Has(noBc.error, "textureCompressionBC"), "CONTROL: DXT5 without textureCompressionBC is refused");
    // GetTiledOffset2D (texture_util.cc:443-458) with 16-byte blocks (log2 4), macro 0:
    //   micro = ((x & 7) + ((y & 0xE) << 2)) << 4; offset = ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4); result = offset.
    //   (0,0): 0. (1,0): micro 16 -> 32. (0,1): 0 + 16 = 16. (1,1): 32 + 16 = 48. Output rows (0,0) (1,0) (0,1) (1,1);
    //   8in16: output byte j of a block is source byte j ^ 1.
    const PreparedTexture s = m.Prepare(kDxt5Small, ShaderFetch{}, false, kFull);
    Expect(s.entry != nullptr && s.plan.stagingBytes == 64, "small DXT5: 2 x 2 blocks of 16 bytes = 64 staging bytes");
    if (s.entry == nullptr)
        return;
    FakeGuest g = Pattern(kSmallBase, s.entry->description.levels[0].extentBytes);
    std::vector<uint8_t> out(64, 0xEE);
    std::string error;
    const uint32_t offsets[4] = {0, 32, 16, 48};
    bool all = FillStaging(s.entry->description, s.plan, GuestOf, &g, out.data(), error);
    for (uint32_t k = 0; all && k < 4; k++)
        for (uint32_t j = 0; j < 16; j++)
            all = out[k * 16 + j] == uint8_t(offsets[k] + (j ^ 1));
    Expect(all, "small DXT5: each 16-byte block from its hand-computed tiled offset (0, 32, 16, 48), 8in16-swapped");
    Expect(out[16] == 33 && out[32] == 17, "small DXT5: block (1,0) comes from 32 and (0,1) from 16 (tiled, not row-major)");
}

// FM4 Recomp: k_16_16 (25), an FM4 texture (slot 0, the k_16_16 resolve's destination). Hand decode
// (xenos.h:1206-1307): 84024956: type 2, signs (0x4956 >> 2) & 0xFF = 0x55 -> 1 1 1 1 (signed), pitch ((1u << 31 | 0x04024956) >> 22) &
// 0x1FF = 0x10 -> 512, tiled. 1C1E3059: format 0x19 = 25, endian (0x59 >> 6) & 3 = 1 (8in16), base 1C1E3000. 003FE1FF: 512 x 512.
// 01063211: number format 1 (ignored: Xenia never reads it), swizzle (0x3211 >> 1) & 0xFFF = 0x1908 -> X Y 0 0, point, base map.
// Xenia vulkan_texture_cache.cc:220-225: R16G16_UNORM, signed R16G16_SNORM (78), RGGG -> R G ZERO ZERO; 512 x 512 x 4 = 1,048,576 bytes.
const uint32_t kK16Signed[6] = {1u << 31 | 0x04024956u, 0x1C1E3059u, 0x003FE1FFu, 0x01063211u, 0x00000000u, 0x00000200u};

void TestK16_16Texture()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kK16Signed, ShaderFetch{}, false, kFull);
    Expect(p.entry != nullptr && p.entry->description.host.vkFormat == 78 && p.plan.stagingBytes == 1048576 &&
               Components(p.binding, vk::kSwizzleR, vk::kSwizzleG, vk::kSwizzleZero, vk::kSwizzleZero),
           "scenario19 k_16_16 texture: signed -> R16G16_SNORM (78), R G 0 0, 1,048,576 bytes");
    uint32_t unsigned16[6];
    std::memcpy(unsigned16, kK16Signed, sizeof unsigned16);
    unsigned16[0] &= ~(0xFFu << 2);  // signs 0 0 0 0
    const PreparedTexture u = m.Prepare(unsigned16, ShaderFetch{}, false, kFull);
    Expect(u.entry != nullptr && u.entry->description.host.vkFormat == 77 && u.entry != p.entry,
           "the same storage unsigned: R16G16_UNORM (77), another cache entry");
    uint32_t mixed[6];
    std::memcpy(mixed, kK16Signed, sizeof mixed);
    mixed[0] &= ~(3u << 2);  // component x unsigned, y z w signed
    Expect(m.Prepare(mixed, ShaderFetch{}, false, kFull).entry == nullptr, "CONTROL: mixed signs on k_16_16 are refused");
    uint32_t signedBase0[6];
    std::memcpy(signedBase0, kK16Signed, sizeof signedBase0);
    signedBase0[1] &= 0xFFFu;  // base address 0 (format and endian kept)
    const PreparedTexture z = m.Prepare(signedBase0, ShaderFetch{}, false, kFull);
    Expect(z.entry == nullptr && Has(z.error, "base address 0"), "a signed k_16_16 constant still meets the base-address checks (base 0 refused)");
}

// FM4 Recomp: k_8_8 (10), the destination of the k_8_8 resolve (no fetch of it observed
// yet). A synthetic tiled 32x32 constant at 0x00100000, pitch 32 texels (1 << 22), endian 8in16 (dword 1 bits 6-7), X Y Z W,
// unsigned. Xenia vulkan_texture_cache.cc:141-145: R8G8_UNORM (VkFormat 16), signed R8G8_SNORM (17), swizzle RGGG, the 16bpb
// load. 2-byte blocks: 32 x 32 x 2 = 2,048 staging bytes; the tiled extent TiledAddressUpperBound2D(32, 32, 32, 1) = 0 + 0xC00
// = 3,072 (texture_util.cc:507-511).
const uint32_t kK88[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 10u | 1u << 6 | kSmallBase, 31u | 31u << 13,
                          kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};

// Anisotropy (X vulkan_texture_cache.cc:671-675 resolve and clamp, :798-802 maxAnisotropy = 1 << (aniso - 1), :2591-2598 the
// device's samplerAnisotropy and limit): the k_DXT1 constant with aniso 3 (dword 3 bits 25-27) -> 4:1.
void TestAnisotropy()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    uint32_t fetch[6];
    std::copy(kDxt1Format18, kDxt1Format18 + 6, fetch);
    fetch[3] |= 3u << 25;
    const PreparedTexture off = m.Prepare(fetch, ShaderFetch{}, false, kFull);  // the device feature not enabled
    Expect(off.entry == nullptr && off.error.find("anisotropic filtering") != std::string::npos, "aniso 3 without samplerAnisotropy is refused");
    const PreparedTexture on = m.Prepare(fetch, ShaderFetch{}, false, TextureDeviceFeatures{true, true, true, 16.0f});
    Expect(on.entry != nullptr && on.binding.sampler.anisotropyEnable && on.binding.sampler.maxAnisotropy == 4.0f, "aniso 3 -> 4:1");
    const PreparedTexture clamped = m.Prepare(fetch, ShaderFetch{}, false, TextureDeviceFeatures{true, true, true, 2.0f});
    Expect(clamped.entry != nullptr && clamped.binding.sampler.maxAnisotropy == 2.0f, "4:1 clamped to the device's 2");
    ShaderFetch instructionOff{};
    instructionOff.aniso = 0;
    const PreparedTexture none = m.Prepare(fetch, instructionOff, false, kFull);
    Expect(none.entry != nullptr && !none.binding.sampler.anisotropyEnable && none.binding.sampler.maxAnisotropy == 1.0f, "the instruction's 0: no anisotropy");
    Expect(!(on.binding.sampler == none.binding.sampler), "CONTROL: the anisotropic sampler is a different key");
}

// FM4 Recomp: k_DXN (49), an FM4 texture (slot 3). Hand decode (xenos.h:1206-1307): 82000002: type 2, signs 0,
// pitch (word 0 >> 22) & 0x1FF = 8 -> 256, tiled. 0DABF071: format 0x31 = 49, endian (0x71 >> 6) & 3 = 1 (8in16). 001FE0FF:
// 256 x 256. 00A80D10: swizzle 0x688 = X Y Z W, mag / min / mip linear. 00000203: mip max (0x203 >> 6) & 0xF = 8 -> 9 levels
// (256 .. 1). 0DAB2A18: 2D, packed mips. Host (Xenia vulkan_texture_cache.cc:330-333): BC5_UNORM (141), RGGG -> R G G G.
const uint32_t kDxnObserved[6] = {1u << 31 | 0x02000002u, 0x0DABF071u, 0x001FE0FFu, 0x00A80D10u, 0x00000203u, 0x0DAB2A18u};
// A tiled DXN 8x8 texels (2x2 blocks of 16 bytes), 8in16, pitch 32 texels, at 0x00100000 (the small DXT5's layout).
const uint32_t kDxnSmall[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 49u | 1u << 6 | kSmallBase, 7u | 7u << 13,
                               kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};
void TestDxn()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture o = m.Prepare(kDxnObserved, ShaderFetch{}, false, kFull);
    Expect(o.entry != nullptr && o.entry->description.host.vkFormat == 141 && o.entry->description.host.blockCompressed &&
               o.entry->description.levelCount == 9 && Components(o.binding, vk::kSwizzleR, vk::kSwizzleG, vk::kSwizzleG, vk::kSwizzleG),
           "scenario28 DXN: k_DXN (49) 256x256, 9 levels, BC5 (141), R G G G");
    if (o.entry == nullptr)
        std::printf("  (%s)\n", o.error.c_str());
    const PreparedTexture noBc = m.Prepare(kDxnSmall, ShaderFetch{}, false, {false, true});
    Expect(noBc.entry == nullptr && Has(noBc.error, "textureCompressionBC"), "CONTROL: DXN without textureCompressionBC is refused");
    uint32_t signedDxn[6];
    std::copy(kDxnSmall, kDxnSmall + 6, signedDxn);
    signedDxn[0] |= 0x55u << 2;  // all four signs 1: Xenia has no signed DXN (:332 kLoadShaderIndexUnknown)
    Expect(m.Prepare(signedDxn, ShaderFetch{}, false, kFull).entry == nullptr, "CONTROL: a signed DXN is refused");
    // The staging bytes as the small DXT5 (16-byte blocks, GetTiledOffset2D log2 4): blocks from 0, 32, 16, 48, 8in16.
    const PreparedTexture s = m.Prepare(kDxnSmall, ShaderFetch{}, false, kFull);
    Expect(s.entry != nullptr && s.plan.stagingBytes == 64, "small DXN: 2 x 2 blocks of 16 bytes = 64 staging bytes");
    if (s.entry == nullptr)
        return;
    FakeGuest g = Pattern(kSmallBase, s.entry->description.levels[0].extentBytes);
    std::vector<uint8_t> out(64, 0xEE);
    std::string error;
    const uint32_t offsets[4] = {0, 32, 16, 48};
    bool all = FillStaging(s.entry->description, s.plan, GuestOf, &g, out.data(), error);
    for (uint32_t k = 0; all && k < 4; k++)
        for (uint32_t j = 0; j < 16; j++)
            all = out[k * 16 + j] == uint8_t(offsets[k] + (j ^ 1));
    Expect(all, "small DXN: each 16-byte block from its hand-computed tiled offset (0, 32, 16, 48), 8in16-swapped");
}

// FM4 Recomp: k_DXT5A (59), an FM4 texture (slot 0). Hand decode: 84004802: tiled, pitch (word 0 >> 22) & 0x1FF
// = 0x10 -> 512, signs 0. 0D0F907B: format 0x3B = 59, endian (0x7B >> 6) & 3 = 1. 007FE1FF: 512 x 1024. 003D4283: mip max
// (0x4283 >> 6) & 0xF = 10 -> 11 levels (1024 .. 1). 0D0E0A18: 2D, packed mips. Host (Xenia vulkan_texture_cache.cc:381-385):
// BC4_UNORM (139), RRRR.
const uint32_t kDxt5aObserved[6] = {1u << 31 | 0x04004802u, 0x0D0F907Bu, 0x007FE1FFu, 0x00A80D10u, 0x003D4283u, 0x0D0E0A18u};
// A tiled DXT5A 8x8 texels (2x2 blocks of 8 bytes), 8in16, pitch 32 texels, at 0x00100000.
const uint32_t kDxt5aSmall[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 59u | 1u << 6 | kSmallBase, 7u | 7u << 13,
                                 kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, kDim2D};
void TestDxt5a()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture o = m.Prepare(kDxt5aObserved, ShaderFetch{}, false, kFull);
    Expect(o.entry != nullptr && o.entry->description.host.vkFormat == 139 && o.entry->description.host.blockCompressed &&
               o.entry->description.levelCount == 11 && Components(o.binding, vk::kSwizzleR, vk::kSwizzleR, vk::kSwizzleR, vk::kSwizzleR),
           "scenario31 DXT5A: k_DXT5A (59) 512x1024, 11 levels, BC4 (139), R R R R");
    if (o.entry == nullptr)
        std::printf("  (%s)\n", o.error.c_str());
    uint32_t signedDxt5a[6];
    std::copy(kDxt5aSmall, kDxt5aSmall + 6, signedDxt5a);
    signedDxt5a[0] |= 0x55u << 2;  // all four signs 1: no signed BC4 in Xenia (:384 kLoadShaderIndexUnknown)
    Expect(m.Prepare(signedDxt5a, ShaderFetch{}, false, kFull).entry == nullptr, "CONTROL: a signed DXT5A is refused");
    // GetTiledOffset2D (texture_util.cc:443-458) with 8-byte blocks (log2 3): micro = ((x & 7) + ((y & 0xE) << 2)) << 3, offset =
    // ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4): (0,0) 0, (1,0) 8, (0,1) 16, (1,1) 24 (linear would put (0,1) at 64).
    const PreparedTexture s = m.Prepare(kDxt5aSmall, ShaderFetch{}, false, kFull);
    Expect(s.entry != nullptr && s.plan.stagingBytes == 32, "small DXT5A: 2 x 2 blocks of 8 bytes = 32 staging bytes");
    if (s.entry == nullptr)
        return;
    FakeGuest g = Pattern(kSmallBase, std::max<size_t>(s.entry->description.levels[0].extentBytes, 64));
    std::vector<uint8_t> out(32, 0xEE);
    std::string error;
    const uint32_t offsets[4] = {0, 8, 16, 24};
    bool all = FillStaging(s.entry->description, s.plan, GuestOf, &g, out.data(), error);
    for (uint32_t k = 0; all && k < 4; k++)
        for (uint32_t j = 0; j < 8; j++)
            all = out[k * 8 + j] == uint8_t(offsets[k] + (j ^ 1));
    Expect(all, "small DXT5A: each 8-byte block from its hand-computed tiled offset (0, 8, 16, 24), 8in16-swapped");
}

// FM4 Recomp: a 1D texture: linear k_8 256 texels at 0x00100000, pitch 256 (8 << 22), dimension 0 (dword 5
// bits 9-10 clear). A height-1 2D image (X texture_util.cc:207-213): 256 x 1 x 1 byte = 256 staging bytes, the row as stored.
const uint32_t kLinear1D[6] = {2u | kClampEdgeXYZ | 8u << 22, 2u | kSmallBase, 255u, kLinearMagMin | kMipBaseMap, 0, 0};
void Test1D()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    ShaderFetch fetch1D{};
    fetch1D.dimension = 0;
    const PreparedTexture p = m.Prepare(kLinear1D, fetch1D, false, kFull);
    Expect(p.entry != nullptr && p.entry->description.width == 256 && p.entry->description.height == 1 && p.entry->description.levelCount == 1 &&
               p.plan.stagingBytes == 256,
           "1D k_8 256 texels: a 256 x 1 image, 256 staging bytes");
    if (p.entry == nullptr)
    {
        std::printf("  (%s)\n", p.error.c_str());
        return;
    }
    FakeGuest g = Pattern(kSmallBase, 256);
    std::vector<uint8_t> out(256, 0xEE);
    std::string error;
    bool row = FillStaging(p.entry->description, p.plan, GuestOf, &g, out.data(), error);
    for (uint32_t i = 0; row && i < 256; i++)
        row = out[i] == uint8_t(i);
    Expect(row, "1D: the row's bytes as stored (no endian swap, linear)");
    Expect(m.Prepare(kLinear1D, ShaderFetch{}, false, kFull).entry == nullptr, "CONTROL: a 2D fetch of a 1D constant is refused");
    // FM4's slot 2: 02020002 117BA086 000000FF 01280C14 00000203 00000A00, a linear 2D constant (dword 5
    // bits 9-10 = (0xA00 >> 9) & 3 = 1), 256 x 1 (000000FF: width 0xFF + 1, height 0 + 1), format 6 (k_8_8_8_8, 0x86 & 0x3F), fetched
    // as 1D: Xenia samples it at (s, 0) through its 2D binding. A 256 x 1 2D image, 256 x 4 = 1024 staging bytes.
    const uint32_t observed[6] = {0x02020002u, 0x117BA086u, 0x000000FFu, 0x01280C14u, 0x00000203u, 0x00000A00u};
    const PreparedTexture o = m.Prepare(observed, fetch1D, false, kFull);
    Expect(o.entry != nullptr && o.entry->description.dimension == 1 && o.entry->description.width == 256 && o.entry->description.height == 1 &&
               o.plan.stagingBytes == 1024,
           "scenario44: a 2D 256 x 1 constant fetched as 1D: its 2D image, 1024 staging bytes");
    if (o.entry == nullptr)
        std::printf("  (%s)\n", o.error.c_str());
}

// FM4 Recomp: k_DXT2_3 (19), an FM4 texture (slot 0): 810048FE (tiled, pitch (word 0 >> 22) & 0x1FF = 4 -> 128,
// signs (0x48FE >> 2) & 0xFF = 0x3F: x y z gamma) 0A21F053 (format 0x13 = 19, endian 1) 0001E01F (32 x 16) 00A80D10 003E8143
// 00000A18 (mip address 0: one level). Host (Xenia vulkan_texture_cache.cc:191-195): BC2_UNORM (135), RGBA, gamma.
void TestDxt23()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const uint32_t observed[6] = {1u << 31 | 0x010048FEu, 0x0A21F053u, 0x0001E01Fu, 0x00A80D10u, 0x003E8143u, 0x00000A18u};
    const PreparedTexture p = m.Prepare(observed, ShaderFetch{}, true, kFull);
    Expect(p.entry != nullptr && p.entry->description.host.vkFormat == 135 && p.entry->description.host.blockCompressed &&
               p.entry->description.levelCount == 1 && p.binding.gamma && p.plan.stagingBytes == 8 * 4 * 16,
           "scenario54 DXT2_3: k_DXT2_3 (19) 32x16, BC2 (135), gamma, 8 x 4 blocks of 16 bytes = 512 staging bytes");
    if (p.entry == nullptr)
        std::printf("  (%s)\n", p.error.c_str());
    Expect(m.Prepare(observed, ShaderFetch{}, true, {false, true}).entry == nullptr, "CONTROL: DXT2_3 without textureCompressionBC is refused");
}

// FM4 Recomp: FM4_TEXTURE_REUSE's content hash (texture_upload.h ContentHash): stable for equal bytes; one flipped bit anywhere (every
// position of a 1,061-byte buffer: 33 whole 32-byte blocks and a 5-byte tail), the length or the seed changes it.
void TestContentHash()
{
    std::vector<uint8_t> a(1061);
    for (size_t i = 0; i < a.size(); i++)
        a[i] = uint8_t(i * 37 + 11);
    const uint64_t h = ContentHash(a.data(), a.size(), 7);
    Expect(ContentHash(a.data(), a.size(), 7) == h, "content hash: equal bytes, equal hash");
    bool all = true;
    for (size_t i = 0; i < a.size() && all; i++)
        for (uint32_t bit = 0; bit < 8 && all; bit++)
        {
            std::vector<uint8_t> b = a;
            b[i] ^= uint8_t(1u << bit);
            all = ContentHash(b.data(), b.size(), 7) != h;
        }
    Expect(all, "content hash: every single-bit change (8,488 of them, blocks and tail) changes it");
    Expect(ContentHash(a.data(), a.size() - 1, 7) != h && ContentHash(a.data(), a.size(), 8) != h, "content hash: the length and the seed change it");
}

// FM4 Recomp: ReservedStagingBytes (texture_upload.h): a reused bind reserves 0 ring bytes; each other case its full staging.
void TestReservedStagingBytes()
{
    Expect(ReservedStagingBytes(true, true, true, 0x1234, 0x1234, 4160) == 0, "ring reservation: an unchanged, hashed image reserves 0 bytes");
    Expect(ReservedStagingBytes(true, true, true, 0x1234, 0x1235, 4160) == 4160 && ReservedStagingBytes(false, true, true, 0x1234, 0x1234, 4160) == 4160 &&
               ReservedStagingBytes(true, false, true, 0x1234, 0x1234, 4160) == 4160 && ReservedStagingBytes(true, true, false, 0x1234, 0x1234, 4160) == 4160,
           "CONTROL: changed bytes, reuse off, a never-hashed image or an unreadable range reserve the full 4,160 bytes");
}

void TestK88Texture()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kK88, ShaderFetch{}, false, kFull);
    // X Y Z W through RGGG (texture_info.cpp DescribeBinding: guest component i reads the host format's swizzle of it): R G G G.
    Expect(p.entry != nullptr && p.entry->description.host.vkFormat == 16 && p.plan.stagingBytes == 2048 &&
               p.entry->description.levels[0].extentBytes == 3072 && Components(p.binding, vk::kSwizzleR, vk::kSwizzleG, vk::kSwizzleG, vk::kSwizzleG),
           "k_8_8 32x32 tiled: R8G8_UNORM (16), R G G G, 2,048 staging bytes from a 3,072-byte extent");
    if (p.entry == nullptr)
        return;
    // The staging bytes: texel (x, y) at (y * 32 + x) * 2 from TiledOffset2D(x, y, 32, 1), bytes swapped (8in16, endian.xesli
    // :25-31). (9, 3) -> 114 (test_edram_vk.cpp TestK88Resolve's hand value), staged at 210; (0, 0) -> 0, staged at 0.
    // The guest bytes there are the resolve's (22 11: the big-endian 16-bit 0x2211 of R 0x11, G 0x22), so R8G8 reads
    // R = 0x11 at 210 and G = 0x22 at 211: the resolve and the texture agree (a wiring check, not an oracle).
    FakeGuest g = Pattern(kSmallBase, 3072);
    g.bytes[114] = 0x22, g.bytes[115] = 0x11;
    std::vector<uint8_t> out(p.plan.stagingBytes, 0xEE);
    std::string error;
    Expect(FillStaging(p.entry->description, p.plan, GuestOf, &g, out.data(), error) && out[0] == 1 && out[1] == 0 && out[210] == 0x11 &&
               out[211] == 0x22,
           "k_8_8 staging: (0,0) from 0 and (9,3) from 114, 8in16-swapped: R 0x11, G 0x22");
    Expect(!(out[210] == 210 && out[211] == 211), "CONTROL: (9,3) is not the row-major source byte 210");
    Expect(!(out[210] == 0x22 && out[211] == 0x11), "CONTROL: the bytes are swapped (endian 1), not copied");
    uint32_t signed88[6];
    std::memcpy(signed88, kK88, sizeof signed88);
    signed88[0] |= 0x55u << 2;  // signs 1 1 1 1
    const PreparedTexture s = m.Prepare(signed88, ShaderFetch{}, false, kFull);
    Expect(s.entry != nullptr && s.entry->description.host.vkFormat == 17 && s.entry != p.entry,
           "k_8_8 every component signed: R8G8_SNORM (17), another cache entry");
}

// FM4 Recomp: a cube (dimension 3, reference/gpu/shader_census_forecast.md: 294 of 491 disc pixel shaders fetch cubes). Hand layout
// (texture_util.cc:42-47, :220-221, :306-344, :425-427): k_8_8_8_8 (4-byte blocks) 32x32 tiled, pitch 32 texels (1 << 22),
// size_2d stack depth 5 (bits 26-31); a face's row pitch 32 x 4 = 128 bytes, its height 32 rows (aligned to 32) -> stride
// 128 x 32 = 4096 (already 4 KB-aligned); faces at base + k x 4096; the staging holds the six faces, 32 x 32 x 4 = 4096 each.
const uint32_t kCube[6] = {2u | kClampEdgeXYZ | 1u << 22 | 1u << 31, 6u | 2u << 6 | 0x00200000u, 31u | 31u << 13 | 5u << 26,
                           kSwizzleXYZW | kLinearMagMin | kMipBaseMap, 0, 3u << 9};

void TestCube()
{
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    ShaderFetch cube{};
    cube.dimension = 3;
    const PreparedTexture p = m.Prepare(kCube, cube, false, kFull);
    Expect(p.entry != nullptr && p.entry->description.layers == 6 && p.entry->description.levels[0].faceStrideBytes == 4096 &&
               p.plan.stagingBytes == 6u * 4096u,
           "cube 32x32 k_8_8_8_8: six layers, faces 4096 bytes apart, 24,576 staging bytes");
    if (p.entry == nullptr)
        return;
    const MipLevel& l = p.entry->description.levels[0];
    // The tiled extent of one face (GetTiledAddressUpperBound2D(32, 32, 32, log2 2): one 32x32 tile of 4-byte blocks = 4096) after
    // the other five faces.
    Expect(l.extentBytes == 5u * 4096u + 4096u, "cube: the extent covers five strides and the last face (24,576 bytes)");
    FakeGuest g{0x00200000u, std::vector<uint8_t>(l.extentBytes, 0)};
    for (uint32_t face = 0; face < 6; face++)
        std::fill(g.bytes.begin() + face * 4096, g.bytes.begin() + face * 4096 + 4096, uint8_t(face + 1));
    std::vector<uint8_t> out(p.plan.stagingBytes, 0xEE);
    std::string error;
    bool order = FillStaging(p.entry->description, p.plan, GuestOf, &g, out.data(), error);
    for (uint32_t face = 0; order && face < 6; face++)
        for (uint32_t i = 0; order && i < 4096; i++)
            order = out[face * 4096 + i] == uint8_t(face + 1);
    Expect(order, "cube: staging face k holds guest face k (+X -X +Y -Y +Z -Z, the host array layers in order)");
    const std::vector<CopyRegion> r = CopyRegions(p.entry->description, p.plan, 0);
    Expect(r.size() == 1 && r[0].layers == 6 && r[0].width == 32 && r[0].height == 32, "cube: one copy region of six layers");
    uint32_t four[6];
    std::memcpy(four, kCube, sizeof four);
    four[2] = (four[2] & 0x03FFFFFFu) | 4u << 26;
    Expect(m.Prepare(four, cube, false, kFull).entry == nullptr, "CONTROL: a cube constant with stack depth 4 (five faces) is refused");
    Expect(m.Prepare(kCube, ShaderFetch{}, false, kFull).entry == nullptr, "CONTROL: a 2D fetch of a cube constant is refused");
    // FM4's cube texture (slot 1): 82024802 (tiled, pitch (word 0 >> 22) & 0x1FF = 8 -> 256) 0DA3C052 (format 0x12 =
    // 18 k_DXT1, endian 1) 141FE0FF (256 x 256, stack depth 0x141FE0FF >> 26 = 5) 00A80D10 00000003 (one level) 00000618 (cube).
    // DXT1: 64 x 64 blocks of 8 bytes: row pitch 512, 64 block rows (aligned to 32) -> face stride 32,768 (4 KB-aligned);
    // staging 6 x 64 x 64 x 8 = 196,608 bytes.
    const uint32_t observed[6] = {1u << 31 | 0x02024802u, 0x0DA3C052u, 0x141FE0FFu, 0x00A80D10u, 0x00000003u, 0x00000618u};
    const PreparedTexture o = m.Prepare(observed, cube, false, kFull);
    Expect(o.entry != nullptr && o.entry->description.host.vkFormat == 133 && o.entry->description.layers == 6 &&
               o.entry->description.levelCount == 1 && o.entry->description.levels[0].faceStrideBytes == 32768 && o.plan.stagingBytes == 196608,
           "scenario29 cube: k_DXT1 256x256, six faces 32,768 bytes apart, BC1, 196,608 staging bytes");
}

void TestGlow()
{
    const uint32_t kGlow[6] = {1u << 31 | 0x064200FEu, 0x117D80B2u, 0x003E631Fu, 0x00280C14u, 0x00000003u, 0x00000A00u};
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kGlow, ShaderFetch{}, true, kFull);
    Expect(p.entry != nullptr, "the logo glow (format 50, 800x500, tiled, 8in32) is accepted");
    if (p.entry == nullptr)
    {
        std::printf("  %s\n", p.error.c_str());
        return;
    }
    const TextureDescription& d = p.entry->description;
    Expect(d.width == 800 && d.height == 500 && d.fetch.tiled && d.fetch.endian == 2 && d.levelCount == 1 && d.packedLevel == 5 &&
               d.levels[0].pitchBlocks == 800 && d.base.physical == 0x117D8000u && p.plan.stagingBytes == 800u * 500u * 4u,
           "glow description: 800x500, one level (packed level 5, base unpacked), pitch 800 blocks, 1,600,000 staged bytes");
    Expect(Components(p.binding, vk::kSwizzleB, vk::kSwizzleG, vk::kSwizzleR, vk::kSwizzleA) && p.binding.gamma,
           "glow view: Z Y X W = B G R A; w is the image's A; gamma on x y z only (w sign 0)");
    const SamplerDescription& s = p.binding.sampler;
    Expect(s.addressModeU == vk::kAddressRepeat && s.addressModeV == vk::kAddressRepeat && s.magFilter == vk::kFilterLinear &&
               s.minFilter == vk::kFilterLinear && s.mipmapMode == vk::kMipmapModeNearest,
           "glow sampler: REPEAT on U and V (clamp 0) with linear filtering: the quad's edge texels blend with the opposite edge");
    FakeGuest g{0x117D8000u, std::vector<uint8_t>(d.levels[0].extentBytes)};
    for (size_t i = 0; i < g.bytes.size(); i++)
        g.bytes[i] = uint8_t(i % 251);
    std::vector<uint8_t> out(p.plan.stagingBytes);
    std::string error;
    const bool filled = FillStaging(d, p.plan, GuestOf, &g, out.data(), error);
    struct Texel { uint32_t x, y, offset; };
    const Texel texels[] = {{0, 0, 0}, {1, 0, 4}, {0, 1, 16}, {8, 0, 64}};
    bool alpha = filled, reversed = filled;
    for (const Texel& t : texels)
    {
        const uint8_t* staged = out.data() + (size_t(t.y) * 800 + t.x) * 4;
        alpha = alpha && staged[3] == g.bytes[t.offset];  // A = the guest's first byte
        for (uint32_t j = 0; j < 4; j++)
            reversed = reversed && staged[j] == g.bytes[t.offset + 3 - j];
    }
    Expect(alpha, "glow alpha: the staged A of texels (0,0) (1,0) (0,1) (8,0) is the guest byte at tiled offset 0 4 16 64 (the word's first byte)");
    Expect(reversed, "glow 8in32: each staged texel is its guest word with the four bytes reversed");
}

}  // namespace

// FM4 Recomp (FM4_RESOLVE_RESIDENT, resolve_resident.h): the traced HDR resolve (the local capture ach1 (not distributed), draws
// 33684-33685: two bands into 1BB71000, 1280x512 then rows 512-719, k01 of a 2x 7e3 surface, k_2_10_10_10, exp bias -1, swap,
// 8in32) and the texture the next draws sample there (kHdr: k_2_10_10_10_AS_16_16_16_16 1280x720, tiled, 8in32, pitch 1280).
// The GPU path copies ResolveConvertPixel's word of rectangle pixel (x, y) to texel (x0 + x, y0 + y) of that texture's image;
// the CPU path stores it into guest memory (ResolveConvertToGuest: endian swap, tiling) and the texture unit loads it from
// there (FillStaging: untiling, endian swap). Both must give the image the same bytes. The words are pseudo-random 7e3 samples;
// the expectation is ResolveConvertPixel of the same samples (test_edram_vk.cpp checks that function by hand), at the texel the
// texture's row-major staging holds at (y * 1280 + x) * 4.
uint32_t BigEndianFloat(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return __builtin_bswap32(bits);
}

edram::ResolveRegisters HdrBand(uint32_t y0, uint32_t y1, uint32_t destBase = 0x1BB71000)
{
    edram::ResolveRegisters r;
    r.surfaceInfo = 0x14010500, r.colorInfo[0] = 0x000C0000, r.depthInfo = 0x00010400, r.modeControl = 6;
    r.copyControl = 0x00100340, r.copyDestBase = destBase, r.copyDestPitch = 0x02D00500, r.copyDestInfo = 0x013F0382;
    r.colorClear = r.colorClearLo = 0x08521485;
    r.windowScissorBr = 0x02D00500, r.screenScissorBr = 0x20002000;
    r.fetch0 = 0x11593203, r.fetch1 = 0x1000001A;
    const float v[6] = {0.0f, float(y0), 1280.0f, float(y0), 1280.0f, float(y1)};
    for (int i = 0; i < 6; i++)
        r.vertexWords[i] = BigEndianFloat(v[i]);
    return r;
}

// One band through both paths; returns the number of rectangle pixels whose staged texel differs from the GPU path's word.
uint64_t ResidentBandMismatches(const edram::ResolveInfo& info, const uint32_t fetch[6], std::string& error, uint32_t cornerX = 0, uint32_t cornerY = 0)
{
    const uint32_t width = info.x1 - info.x0, height = info.y1 - info.y0;
    const DumpLayout l = DumpLayoutFor(info.msaa, width, height);
    std::vector<uint32_t> words(size_t(l.width) * l.columns * l.height * l.rows);
    uint32_t seed = 0x12345678u;
    for (uint32_t& w : words)
        w = (seed = seed * 1664525u + 1013904223u);
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(fetch, ShaderFetch{}, false, kFull);
    if (p.entry == nullptr)
    {
        error = p.error;
        return ~0ull;
    }
    const TextureDescription& d = p.entry->description;
    FakeGuest g{d.base.physical, std::vector<uint8_t>(d.levels[0].extentBytes, 0)};
    if (info.destExtentStart < g.physical || info.destExtentStart - g.physical + uint64_t(info.destExtentLength) > g.bytes.size() ||
        !ResolveConvertToGuest(info, 3, words.data(), g.bytes.data() + (info.destExtentStart - g.physical), info.destExtentLength, error))
        return ~0ull;
    std::vector<uint8_t> out(p.plan.stagingBytes);
    if (!FillStaging(d, p.plan, GuestOf, &g, out.data(), error))
        return ~0ull;
    uint64_t mismatches = 0;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t samples[4] = {};
            for (uint32_t s = 0; s < l.samples; s++)
                samples[s] = words[l.Word(x, y, s)];
            uint32_t staged;
            std::memcpy(&staged, out.data() + (size_t(cornerY + info.y0 + y) * d.width + cornerX + info.x0 + x) * 4, 4);
            mismatches += staged != ResolveConvertPixel(info, 3, samples);
        }
    return mismatches;
}

void TestResidentResolve()
{
    std::string error;
    const edram::ResolveInfo top = edram::DecodeResolve(HdrBand(0, 512)), bottom = edram::DecodeResolve(HdrBand(512, 720));
    Expect(top.ok && top.copy && top.x0 == 0 && top.y0 == 0 && top.x1 == 1280 && top.y1 == 512 && top.destFormat == 7 && top.destEndian == 2 &&
               top.msaa == edram::Msaa::X2 && ResidentConvertible(3, top.destFormat, top.destBytesLog2),
           "resident: the HDR resolve's top band decodes to 1280x512 at 0,0, format 7, 8in32, 2x, convertible");
    Expect(bottom.ok && bottom.copy && bottom.y0 == 512 && bottom.y1 == 720 && bottom.destBase != 0x1BB71000u,
           "resident: the bottom band decodes to rows 512-719, its tile base past the texture's base");
    Expect(ResidentBandMismatches(top, kHdr, error) == 0, "resident: top band, CPU write-back then texture load == the GPU word at (x0 + x, y0 + y), every pixel");
    Expect(ResidentBandMismatches(bottom, kHdr, error) == 0, "resident: bottom band (y0 512): the same, every pixel");
    // CONTROL: the texture read with endian 0 (no swap) does not reproduce the words: the endian equality matters.
    uint32_t noSwap[6];
    std::memcpy(noSwap, kHdr, sizeof noSwap);
    noSwap[1] &= ~(3u << 6);
    const uint64_t wrong = ResidentBandMismatches(top, noSwap, error);
    Expect(wrong != 0 && wrong != ~0ull, "CONTROL: a texture without the 8in32 swap stages other words");

    // The image match (ResidentTextureMatches) on kHdr's shape, then one field off at a time.
    TextureUnitModel m(TextureValidity::ReloadEveryBind);
    const PreparedTexture p = m.Prepare(kHdr, ShaderFetch{}, false, kFull);
    if (p.entry == nullptr)
        return Expect(false, "resident: kHdr prepares");
    const TextureDescription& d = p.entry->description;
    const ResidentTextureShape shape{d.base.physical, d.width, d.height, d.fetch.pitchTexels, d.fetch.format, d.fetch.endian, d.levelCount,
                                     d.dimension, d.depth, d.fetch.tiled, d.fetch.stacked, d.fetch.packedMips};
    ResidentResolve r{true, bottom.destExtentStart, bottom.destExtentLength, 0x1BB71000u, 7, 1280, 2, 0, 512, 1280, 208};
    Expect(ResidentTextureMatches(shape, r), "resident: the bottom band's words go to kHdr's image (format 7 into 54, 8in32, pitch 1280)");
    std::string why;
    ResidentResolve x = r;
    x.converted = false;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "the resolve has no GPU words", "CONTROL: a resolve without GPU words");
    x = r, x.textureBase += 0x800;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "another base", "CONTROL: a base delta that is not a whole 32x32 tile");
    x = r, x.textureBase -= 0x1000;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "another base", "CONTROL: a base below the texture's");

    // The race's HDR bands: RB_COPY_DEST_BASE moved down by whole tile rows, the rectangle at 0,0. Rows
    // 256-511 of a pitch-1280 32 bpp texture start at macro tile (256 >> 5) * (1280 >> 5) = 320, so the base is 1BB71000 +
    // (320 << 12) = 1BCB1000 (the hand value of GetTiledOffset2D (0, 256): macro 320 << 9 = 0x28000, << 3 = 0x140000).
    const edram::ResolveInfo moved = edram::DecodeResolve(HdrBand(0, 256, 0x1BB71000u + 0x140000u));
    Expect(moved.ok && moved.copy && moved.y0 == 0 && moved.y1 == 256 && moved.destExtentStart == 0x1BCB1000u,
           "resident: a band at 1BCB1000, rows 0-255 of its own base");
    Expect(ResidentBandMismatches(moved, kHdr, error, 0, 256) == 0, "resident: that band's CPU write-back read through kHdr == the GPU word at texel (x, 256 + y)");
    const uint64_t shifted = ResidentBandMismatches(moved, kHdr, error, 0, 0);
    Expect(shifted != 0 && shifted != ~0ull, "CONTROL: the same words at texel (x, y) do not match (the corner matters)");
    uint32_t cx = 99, cy = 99;
    x = r, x.textureBase = 0x1BB71000u + 0x140000u, x.y0 = 0, x.height = 256;
    Expect(ResidentTextureMatches(shape, x, &why, &cx, &cy) && cx == 0 && cy == 256, "resident: the moved base's corner is texel (0, 256)");
    x.textureBase = 0x1BB71000u + (41u << 12);  // macro 41: tile (1, 1) -> (32, 32)
    x.width = 32, x.height = 32;
    Expect(ResidentTextureMatches(shape, x, &why, &cx, &cy) && cx == 32 && cy == 32, "resident: macro tile 41 at pitch 1280 is texel (32, 32)");
    x.textureBase = 0x1BB71000u + (320u << 12), x.width = 1280, x.height = 465;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "the rectangle exceeds the texture", "CONTROL: rows 256 + 465 pass the 720-row texture");
    x = r, x.destPitch = 1536;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "another pitch", "CONTROL: another pitch");
    x = r, x.destEndian = 0;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "another endian", "CONTROL: another endian");
    x = r, x.destFormat = 6;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "a format whose texel is not the resolve's word", "CONTROL: an 8_8_8_8 resolve into a 2_10_10_10 texture");
    x = r, x.height = 209;
    Expect(!ResidentTextureMatches(shape, x, &why) && why == "the rectangle exceeds the texture", "CONTROL: one row past the texture");
    ResidentTextureShape t = shape;
    t.levels = 2;
    Expect(!ResidentTextureMatches(t, r, &why) && why == "more than one level", "CONTROL: a texture with mips");
    t = shape, t.tiled = false;
    Expect(!ResidentTextureMatches(t, r, &why) && why == "linear", "CONTROL: a linear texture");
    Expect(ResidentFormatPair(6, 50) && ResidentFormatPair(54, 7) && !ResidentFormatPair(7, 6) && !ResidentFormatPair(36, 36) &&
               ResidentConvertible(0, 6, 2) && !ResidentConvertible(4, 25, 2) && !ResidentConvertible(3, 10, 1) && !ResidentConvertible(14, 36, 2),
           "resident: format pairs (6/50, 7/54) and the convertible storages and formats");

    // The earlier-pending query (texture_upload.h GuestWriteLog::OverlapsEarlier): the last mark does not count.
    GuestWriteLog log;
    log.Mark(0x1BB71000u, 0x1000);
    Expect(!log.OverlapsEarlier(0x1BB71000u, 1), "resident: one mark: nothing earlier overlaps");
    log.Mark(0x1C000000u, 0x1000);
    Expect(log.OverlapsEarlier(0x1BB71000u, 1) && !log.OverlapsEarlier(0x1C000000u, 1), "resident: two marks: only the first counts as earlier");
}

// FM4 Recomp (FM4_WRITE_WATCH): with a protecting backend the cache's own bookkeeping protects nothing (a run stopped
// at draw 3843 when Loaded protected through it), arming goes through the second watch, and GPU writes move both.
struct CountingBackend final : WatchBackend
{
    int calls = 0;
    bool Protect(uint32_t, uint32_t, bool) override { return ++calls, true; }
};
void TestProtectingWatch()
{
    CountingBackend b;
    TextureUnitModel m(TextureValidity::ReloadEveryBind, &b);
    PreparedTexture p = m.Prepare(kUi, ShaderFetch{}, true, kFull);  // the UI constant is gamma (DrawState's define then agrees)
    Expect(p.entry != nullptr && m.Loaded(*p.entry, p.plan) && b.calls == 0, "protecting watch: the cache's Loaded calls no backend");
    Expect(m.ProtectingWatch() != nullptr && m.ProtectingWatch()->Watch(0x11A8F000u, 16384) && b.calls == 1, "protecting watch: arming protects through it");
    const uint64_t before = m.ProtectingWatch()->WriteSequence(0x11A8F000u, 4096);
    m.MarkWritten(0x11A8F000u, 64);
    Expect(m.ProtectingWatch()->WriteSequence(0x11A8F000u, 4096) > before && m.Watch().WriteSequence(0x11A8F000u, 4096) > 0,
           "protecting watch: a GPU write moves both watches' sequences");
    TextureUnitModel plain(TextureValidity::ReloadEveryBind);
    Expect(plain.ProtectingWatch() == nullptr, "CONTROL: without a backend there is no protecting watch");
}

int main()
{
    TestStagingPlacement();
    TestObservedConstants();
    TestRefusals();
    TestValidity();
    TestGuestWriteLog();
    TestReadsPending();
    TestStagingBytes();
    Test3D();
    TestMips();
    TestGlow();
    TestDepthFloat();
    TestDxt5();
    TestK16_16Texture();
    TestK88Texture();
    TestAnisotropy();
    TestDxn();
    TestDxt5a();
    Test1D();
    TestDxt23();
    TestContentHash();
    TestReservedStagingBytes();
    TestCube();
    TestResidentResolve();
    TestProtectingWatch();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
