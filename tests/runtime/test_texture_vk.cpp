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
// Format 18 (k_DXT1): mapped (texture_info.cpp) but not in the observed set: the observed-set control.
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
    p = m.Prepare(kDxt1Format18, shader, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "observed set"), "CONTROL: format 18 (mapped, not observed) is refused, named");
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
    Expect(p.entry == nullptr && Has(p.error, "dimension 3"), "CONTROL: a cube constant is refused");
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

int main()
{
    TestStagingPlacement();
    TestObservedConstants();
    TestRefusals();
    TestValidity();
    TestGuestWriteLog();
    TestStagingBytes();
    Test3D();
    TestMips();
    TestGlow();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
