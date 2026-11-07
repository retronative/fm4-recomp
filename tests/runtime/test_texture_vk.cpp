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
    p = m.Prepare(kComp, shader, true, {true, false});
    Expect(p.entry == nullptr && Has(p.error, "imageViewFormatSwizzle"), "CONTROL: B G R A without imageViewFormatSwizzle is refused");
    p = m.Prepare(kUi, shader, true, {true, false});
    Expect(p.entry != nullptr, "an identity mapping needs no imageViewFormatSwizzle");
    p = m.Prepare(kUi, shader, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "gamma"), "CONTROL: a gamma decision that disagrees with DrawState's is refused");
    p = m.Prepare(kRgba8Format6, shader, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "observed set"), "CONTROL: format 6 (mapped, not observed) is refused, named");
    p = m.Prepare(kDxt1Mips, shader, false, kFull);
    Expect(p.entry == nullptr && Has(p.error, "9 levels"), "CONTROL: a 9-level texture is refused (one level only so far)");
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

}  // namespace

int main()
{
    TestStagingPlacement();
    TestObservedConstants();
    TestRefusals();
    TestValidity();
    TestGuestWriteLog();
    TestStagingBytes();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
